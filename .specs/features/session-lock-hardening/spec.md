# Session Lock Hardening — Feature Specification

## Purpose

Prevent disconnected or frozen MCP sessions from making a project's Axon unavailable while
preserving local-first DuckDB ownership and safe process recovery.

## Scope

- Release a stdio server's DuckDB lock after bounded tool inactivity without exiting the server.
- Reacquire the database transparently on the next tool call.
- Bound localhost peer calls and queue-drain subprocess attempts.
- Persist owner start and heartbeat timestamps in the existing registry.
- Provide read-only lock-owner diagnostics.
- Add deterministic registry, lifecycle, frozen-owner, and hook timeout tests.

## Out of Scope

- Killing live processes, automatic OS-level lock breaking, or PID-reuse heuristics.
- Replacing DuckDB, introducing a daemon coordinator, or changing HTTP service defaults.
- Treating registry metadata as stronger authority than the actual DuckDB lock.
- Releasing the embedding model on idle.

## Functional Requirements

- **FR-01:** stdio `axon serve` MUST close its database after 300 idle seconds by default.
- **FR-02:** an idle release MUST NOT terminate the MCP process or unload its model.
- **FR-03:** the next DB-backed tool call MUST attempt to reacquire and re-register ownership.
- **FR-04:** the idle reaper MUST NOT close the database during an active tool call.
- **FR-05:** a peer call MUST fail within a bounded configurable deadline.
- **FR-06:** the queue-drain hook MUST terminate only its own wedged child after a bounded deadline.
- **FR-07:** `axon doctor locks` MUST classify dead, unresponsive, identity-mismatched, released, and
  healthy owners without exposing authentication tokens.
- **FR-08:** Axon MUST NOT kill or evict a live unresponsive owner automatically.
- **FR-09:** owner registration MUST record start and heartbeat epoch timestamps additively.

## Non-Functional Requirements

- **NFR-01 Reliability:** release, unregister, and reacquire operations are idempotent.
- **NFR-02 Safety:** tool execution and lease release are serialized by the existing mutex.
- **NFR-03 Performance:** heartbeat writes occur no more than once per sixty seconds; health probes
  use a one-second deadline.
- **NFR-04 Security:** peer tokens remain registry-only and never appear in doctor output or logs.
- **NFR-05 Compatibility:** old registry entries load with zero timestamps; existing CLI/MCP
  contracts remain valid.
- **NFR-06 Portability:** deadlines work on POSIX and Windows without a new dependency.
- **NFR-07 Observability:** idle release emits a structured, content-free local lifecycle event.

## Constraints

- C++20, nlohmann/json, existing Bash and PowerShell conventions only.
- No new external service or dependency.
- The dirty primary checkout remains untouched; implementation is isolated in a feature worktree.
