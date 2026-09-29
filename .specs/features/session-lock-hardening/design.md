# Session Lock Hardening — Design

## Ownership Model

The DuckDB file lock remains the source of truth. Registry ownership is discoverability metadata
for peer proxying. A stdio server's lock is modeled as a renewable local lease:

1. database open succeeds;
2. the process registers PID, port, token, start time, and heartbeat;
3. tool calls renew in-memory activity and periodically persist a heartbeat;
4. a reaper sharing `tool_mutex` closes the database after the idle threshold;
5. conditional unregister removes only that PID's metadata;
6. `ensure_db_open` reacquires on a later call.

HTTP servers keep their existing continuous service lifecycle; the idle lease applies only to
stdio sessions, where detached parent processes caused the observed problem.

## Failure Boundaries

- Peer tool calls: `AXON_PEER_TIMEOUT_MS`, default 15000, clamped to 100–300000 ms.
- Health diagnostics: fixed 1000 ms to keep multi-owner inspection bounded.
- Queue-drain child: `AXON_QUEUE_ATTEMPT_TIMEOUT_SECONDS`, default 30 seconds.
- Idle DB lease: `AXON_DB_IDLE_SECONDS`, default 300; `0` disables release for controlled cases.

No timeout authorizes terminating an unrelated process. The Bash and PowerShell hooks only stop
the child process they created.

## Concurrency

The existing `tool_mutex` serializes stdio calls, peer calls, telemetry DB writes, health reads,
and the idle reaper. The reaper therefore cannot reset `ctx.db` while a tool is executing. The peer
listener stays available after release and reports `db_ready=false` during the small transition
window.

## Diagnostics Contract

`axon doctor locks --json` returns only:

- root, PID, peer port;
- process liveness;
- heartbeat age;
- status and a token-free detail;
- idle seconds for a healthy responsive owner.

It performs no registry pruning or process mutation. `axon registry prune` remains the explicit
metadata cleanup command.

## Observability

Idle release emits a single structured stderr record with service, environment, correlation ID,
event, reason, and idle seconds. Source paths, tool arguments, registry tokens, and user content are
excluded.

## Rollout and Rollback

The registry schema is additive and old binaries ignore the timestamp fields. Rollback is binary
only; no database migration is involved. Environment variables allow operators to restore an
effectively permanent lease or tune deadlines while gathering evidence.
