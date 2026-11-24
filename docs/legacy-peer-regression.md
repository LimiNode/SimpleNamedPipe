# Legacy Peer Read Ownership Regression

## Evidence

MegaConnector 1.35.22 uses the IOCP server from SimpleNamedPipe at `e3810ac`.
Its previous in-project server used a separate synchronous read loop. A raw
Win32 message-mode client reproduces the legacy MetaTrader transport: each
UTF-8 JSON document is one pipe message, without a NUL or length prefix.
There is no platform object, authentication, network request or trade command.

The IOCP loop used to fall through after a **write** completion and call
`post_next_read()` even though the prior read was still outstanding. This resets
and reuses both its OVERLAPPED and its buffer before retirement. Win32 requires
independent, stable storage for each outstanding operation. Repeated outbound
balance/connection/heartbeat writes accumulated concurrent reads sharing storage.

`legacy_peer_test` wraps real ReadFile/GetQueuedCompletionStatus calls to audit
that invariant without replacing the OS or injecting failures. On `e3810ac`:

- Three sessions and 30 ping/pong exchanges recorded 36 overlapping reads.
- Adding a burst of 40 distinct non-trading JSON observations failed with
  `burst observations corrupted, reordered or duplicated` (12 overlaps in the
  first session). This is actual data corruption, not only a theoretical race.

The fix retires write completions in their own branch. Only a read/connect
completion can advance the read chain. It changes no wire protocol, heartbeat
interval, broker semantics, retry behavior or public API.

## Verification

Build with `SIMPLE_NAMED_PIPE_BUILD_TESTS=ON`. Run `legacy_peer_test` and the
existing eight `client_server_test` scenarios. The audited target always builds
header-only so the wrappers observe the implementation; the existing suite
also exercises the separately compiled server when static mode is enabled.

The focused test checks initial balance/connection frames, 30 heartbeat replies,
120 distinct burst messages, three client-initiated disconnects/reconnects,
shutdown with an active peer, and repeated stop. Expected summary:

```text
connections=4 disconnects=4 pongs=30 overlapping_reads=0
```

Use `legacy_peer_test --real-period` for the same test with the actual 15-second
server heartbeat interval (about 7.5 minutes). All waits other than that explicit
period are bounded. Local MSVC Debug (static client/server suite) and MinGW GCC
15.2 (header-only suite) pass the fast tests. Remote exact-head results and the
long-period result belong in the PR report, not an assumed success here.

## Limits

This proves and fixes a transport defect in the release dependency. It does
**not** by itself prove the cause of a specific user's disconnection. The short
original test still exchanged its pongs despite violating read ownership.
Correlate the user's Expert/connector journal and disconnect timing before
claiming the reported symptom is resolved. No user release or live account test
is authorized by this change.
