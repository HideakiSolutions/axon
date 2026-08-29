# ADR-0002 — Session Lock Lifecycle

**Status:** Accepted

**Date:** 2026-08-29

**Authors:** Hideaki Solutions Engineering

**Approved by:** Project owner, explicit implementation authorization on 2026-08-29

## Context

An MCP client can remain alive after an SSH or network disconnect and keep Axon's stdin pipe open.
The associated `axon serve` process then retains DuckDB's single-writer lock indefinitely even
though it is not receiving tool calls. A later session normally proxies to that owner, but a live
and frozen owner previously inherited a 300-second socket timeout. Queue-drain retries could stack
three such waits without an outer process deadline.

PID liveness alone cannot distinguish useful work from a frozen process, and automatically killing
a live PID would violate the existing fail-safe recovery policy.

## Decision

- A stdio server owns a renewable, in-process DuckDB lease. The lease is refreshed by tool
  activity and by a bounded registry heartbeat.
- After 300 seconds without a tool call, configurable with `AXON_DB_IDLE_SECONDS`, the owner closes
  its DuckDB handle, clears its graph, and unregisters itself. The MCP process and embedding model
  stay warm; the next tool call reacquires the database.
- Peer calls use a 15-second socket deadline, configurable with `AXON_PEER_TIMEOUT_MS`, instead of
  the former 300-second wait.
- `axon doctor locks [--json]` reports registry owner liveness, responsiveness, identity, heartbeat
  age, and idle time without exposing owner tokens or mutating state.
- Queue-drain subprocesses have an independent 30-second per-attempt deadline, configurable with
  `AXON_QUEUE_ATTEMPT_TIMEOUT_SECONDS`.
- Axon never terminates or automatically evicts a live process merely because it is unresponsive.

## Alternatives Considered

### Kill owners after a stale heartbeat

Rejected because a long-running tool or temporarily stopped process may still legitimately own
the database. PID termination is destructive and a PID can be reused.

### Keep the lock forever and rely only on peer proxying

Rejected because a disconnected parent can remain alive indefinitely, while a frozen peer makes
every later session depend on its health.

### Move to a client/server database

Rejected because it adds infrastructure and abandons Axon's local-first single-file architecture
for a lifecycle problem that can be solved inside the existing process boundary.

## Consequences

### Positive

- Detached idle sessions stop blocking new sessions without being killed.
- Frozen owners fail fast and become directly diagnosable.
- A released server reacquires transparently on its next tool call.
- Registry heartbeat metadata provides bounded operational evidence.

### Negative / Trade-offs

- The first tool call after idle release pays DuckDB reopen and graph-load latency.
- A live but frozen process is diagnosed, not forcibly recovered; operator intervention may still
  be required until its own idle reaper can run.
- Registry heartbeat writes add low-frequency local I/O while a database is owned.

### Risks and Mitigations

- **Release during active work:** tool execution and the idle reaper share `tool_mutex`.
- **Stale process clears a successor:** owner removal remains PID-conditional.
- **Token disclosure:** health and doctor output never serialize `owner_token`.
- **Platform drift:** socket deadlines have Windows and POSIX implementations; lifecycle smokes run
  on POSIX while Windows retains compilation and PowerShell timeout coverage in CI.

## Affected Standards

- Enterprise Constitution §§2.2–2.5, 5, 7, 10 and 14.
- Resilience Patterns Standard — bounded waits, fail-safe recovery, idempotent reacquisition.
- Advanced Testing Strategy Standard — unit and process-level regression coverage.
- Observability Playbook — structured lifecycle event and secret-free diagnostics.
- C++ Architecture, Build & Toolchain and Testing Standards.

This ADR introduces no exception or standards deviation.
