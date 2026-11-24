#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace audit {
std::mutex mutex;
std::map<OVERLAPPED*, bool> reads;
std::atomic<int> reused_reads{0};

BOOL read(HANDLE pipe, void* buffer, DWORD size, DWORD* bytes, OVERLAPPED* ov) {
    if (!ov) return ::ReadFile(pipe, buffer, size, bytes, ov);
    std::lock_guard<std::mutex> lock(mutex);
    if (reads[ov]) ++reused_reads;
    reads[ov] = true;
    const BOOL ok = ::ReadFile(pipe, buffer, size, bytes, ov);
    const DWORD error = GetLastError();
    if (!ok && error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) reads[ov] = false;
    SetLastError(error);
    return ok;
}

BOOL complete(HANDLE port, DWORD* bytes, ULONG_PTR* key, OVERLAPPED** ov, DWORD timeout) {
    const BOOL ok = ::GetQueuedCompletionStatus(port, bytes, key, ov, timeout);
    const DWORD error = GetLastError();
    if (*ov) {
        std::lock_guard<std::mutex> lock(mutex);
        reads[*ov] = false;
    }
    SetLastError(error);
    return ok;
}
}

// Audit real OS calls; do not inject errors or alter completion ordering.
#define ReadFile audit::read
#define GetQueuedCompletionStatus audit::complete
#include <SimpleNamedPipe/NamedPipeServer.hpp>
#undef GetQueuedCompletionStatus
#undef ReadFile

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Predicate>
bool wait_for(Predicate predicate, int milliseconds = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

class LegacyPeer {
public:
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ~LegacyPeer() { close(); }
    void close() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
    void connect(const std::string& name) {
        const std::string path = "\\\\.\\pipe\\" + name;
        require(wait_for([&] {
            pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
            return pipe != INVALID_HANDLE_VALUE;
        }), "legacy peer could not connect");
        DWORD mode = PIPE_READMODE_MESSAGE;
        require(SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != 0, "message mode failed");
    }
    void send(const std::string& message) {
        DWORD bytes = 0;
        require(WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &bytes, nullptr) &&
                bytes == message.size(), "legacy peer write failed");
    }
    std::string receive() {
        require(wait_for([&] {
            DWORD available = 0;
            return PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0;
        }), "legacy peer stopped receiving heartbeat/data");
        char buffer[4096];
        DWORD bytes = 0;
        require(ReadFile(pipe, buffer, sizeof(buffer), &bytes, nullptr) != 0, "legacy peer read failed");
        return std::string(buffer, bytes);
    }
};
}

int main(int argc, char** argv) {
    try {
        const bool real_period = argc > 1 && std::string(argv[1]) == "--real-period";
        const std::string name = "MgcLegacyRegression_" + std::to_string(GetCurrentProcessId());
        std::atomic<int> connected{0}, disconnected{0}, received{0}, client{-1};
        std::mutex messages_mutex;
        std::vector<std::string> messages;
        SimpleNamedPipe::NamedPipeServer server{SimpleNamedPipe::ServerConfig(name)};
        server.on_connected = [&](int id) {
            client = id;
            ++connected;
            server.send_to(id, "{\"b\":1000,\"rub\":0,\"demo\":1}");
            server.send_to(id, "{\"conn\":1,\"aid\":1}");
        };
        server.on_disconnected = [&](int, const std::error_code&) { ++disconnected; };
        server.on_message = [&](int, const std::string& message) {
            if (message == "{\"pong\":1}") ++received;
            else {
                std::lock_guard<std::mutex> lock(messages_mutex);
                messages.push_back(message);
            }
        };
        server.start();
        LegacyPeer peer;
        for (int session = 0; session < 3; ++session) {
            peer.connect(name);
            require(peer.receive() == "{\"b\":1000,\"rub\":0,\"demo\":1}", "balance framing changed");
            require(peer.receive() == "{\"conn\":1,\"aid\":1}", "connection framing changed");
            for (int beat = 0; beat < 10; ++beat) {
                if (real_period) std::this_thread::sleep_for(std::chrono::seconds(15));
                server.send_to(client, "{\"ping\":1}");
                require(peer.receive() == "{\"ping\":1}", "heartbeat framing changed");
                peer.send("{\"pong\":1}");
                require(wait_for([&] { return received == session * 10 + beat + 1; }), "pong lost or duplicated");
            }
            require(connected == session + 1 && disconnected == session, "false connection/disconnection event");
            std::vector<std::string> expected;
            for (int observation = 0; observation < 40; ++observation) {
                const auto message = "{\"test_observation\":" + std::to_string(observation) + "}";
                expected.push_back(message);
                peer.send(message);
            }
            require(wait_for([&] {
                std::lock_guard<std::mutex> lock(messages_mutex);
                return messages.size() >= expected.size();
            }), "burst observations lost");
            {
                std::lock_guard<std::mutex> lock(messages_mutex);
                require(messages == expected, "burst observations corrupted, reordered or duplicated");
                messages.clear();
            }
            peer.close();
            require(wait_for([&] { return disconnected == session + 1; }), "real disconnect was not reported");
        }
        peer.connect(name);
        peer.receive();
        peer.receive();
        server.stop();
        require(wait_for([&] {
            DWORD available = 0;
            return PeekNamedPipe(peer.pipe, nullptr, 0, nullptr, &available, nullptr) == 0;
        }), "server shutdown left a false connected peer");
        require(connected == 4 && disconnected == 4, "shutdown connection events disagree");
        server.stop();
        std::cout << "connections=" << connected << " disconnects=" << disconnected
                  << " pongs=" << received << " overlapping_reads=" << audit::reused_reads << '\n';
        require(audit::reused_reads == 0, "server reuses a pending read OVERLAPPED after write completion");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "; overlapping_reads=" << audit::reused_reads << '\n';
        return 1;
    }
}
