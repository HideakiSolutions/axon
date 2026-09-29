# Native Memory Hardening — Feature Specification

## Purpose

Strengthen Axon's existing local memory path with durable capture, idempotent sessions, typed
cross-agent handoffs, and deterministic hybrid retrieval. The capability is implemented inside
Axon and reuses its DuckDB, MCP, telemetry, and Dialogue Layer boundaries.

## Scope

- Make the existing pending-write queue at-least-once and crash recoverable.
- Add optional idempotency keys to session creation without changing existing callers.
- Add a project-scoped typed handoff lifecycle to the Dialogue Layer.
- Fuse semantic and lexical observation retrieval with Reciprocal Rank Fusion (RRF).
- Add a bounded authority signal to observations and expose ranking evidence.
- Add deterministic unit, integration, smoke, and evaluation coverage.
- Reconcile English and Portuguese public documentation.

## Out of Scope

- A sidecar process, external broker, new database, or network service.
- Replacing the second-brain vault, Qdrant, FalkorDB, or HSEOS governance.
- Automatic approval or mutation of canonical Markdown memory.
- Multi-tenant authorization or using Axon as a security boundary.
- DuckDB VSS/HNSW adoption, a file-watcher rewrite, or LLM-based reranking.
- Migrating or rewriting existing rows.

## Actors

- MCP clients that capture edits and query project memory.
- Coding agents that create, claim, and complete project-scoped handoffs.
- Operators diagnosing queue failures and retrieval quality.
- Existing Axon clients that must remain compatible.

## Functional Requirements

- **FR-01:** Axon MUST retain a claimed pending-write batch until indexing succeeds.
- **FR-02:** Axon MUST recover an interrupted pending-write claim on the next drain.
- **FR-03:** Replaying a pending-write batch MUST be safe and MUST deduplicate paths.
- **FR-04:** `session_start` MUST accept an optional `idempotency_key`; the same thread and key MUST
  return the original session.
- **FR-05:** Existing `session_start` callers without a key MUST retain current behavior.
- **FR-06:** Axon MUST provide typed handoff create, list, claim, get, complete, and cancel operations.
- **FR-07:** Handoff state transitions MUST reject invalid transitions and cross-project working
  directories.
- **FR-08:** Handoff creation MUST support an optional idempotency key.
- **FR-09:** `search_memory` MUST fuse semantic and lexical rankings with deterministic RRF.
- **FR-10:** Observation authority MUST default to `1.0`, be bounded to `[0.5, 2.0]`, and affect
  ranking only after RRF fusion.
- **FR-11:** Search results MUST expose observation id, channel ranks, RRF score, and effective
  authority so ranking is explainable.
- **FR-12:** All MCP contract changes MUST be additive and preserve existing required fields.
- **FR-13:** Axon MUST NOT automatically approve or publish memory changes outside its local index.

## Non-Functional Requirements

- **NFR-01 Reliability:** capture uses at-least-once processing; duplicate delivery is expected and
  indexing remains idempotent, following the Resilience Patterns Standard.
- **NFR-02 Determinism:** identical data and query produce identical fused order, with stable id
  tie-breaking and no LLM in the hot path.
- **NFR-03 Security:** MCP inputs are length-bounded and allowlisted where enumerated; handoff paths
  are confined to the configured project root.
- **NFR-04 Observability:** drain recovery/failure and handoff transitions emit structured local
  telemetry without content, secrets, or PII.
- **NFR-05 Compatibility:** schema evolution is additive; existing databases upgrade in place and
  existing tools keep their previous required inputs.
- **NFR-06 Performance:** hybrid search bounds each candidate channel to at most 500 rows and avoids
  external I/O.
- **NFR-07 Testing:** new ranking and state-machine rules receive deterministic unit tests; DuckDB
  schema/flows receive integration tests; queue crash recovery receives a smoke test.
- **NFR-08 Local-first:** all durable state remains under the project `.axon/` boundary.

## Constraints

- C++20, DuckDB, nlohmann/json, and existing Axon build conventions only.
- No new third-party dependency.
- Build and tests execute sequentially with at most two compilation jobs.
- The published `v1.2.16` tag remains immutable.
- The untracked `.claude/` and `_graph/` content in the primary checkout is not modified.

## Open Questions

None. The project owner explicitly selected native integration over a sidecar on 2026-08-29.
Authority remains a bounded ranking hint, not an authorization decision.
