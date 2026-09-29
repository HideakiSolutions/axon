# ADR-0001 — Native Memory Hardening

**Status:** Accepted

**Date:** 2026-08-29

**Authors:** Hideaki Solutions Engineering

**Approved by:** Project owner, explicit session authorization on 2026-08-29

## Context

Axon already owns project-local code indexing, observations, sessions, turns, hooks, and retrieval.
The evaluated `akitaonrails/ai-memory` repository demonstrates useful patterns for durable capture,
typed handoffs, and hybrid ranking. Running it as a sidecar would create a second lifecycle and
persistence path beside Axon's existing DuckDB and Dialogue Layer.

The Enterprise Constitution requires preserving current architecture, local-first memory,
deterministic verification, additive contracts, and explicit decisions for architecture, schema,
and hot-path ranking changes.

## Decision

Axon will implement the selected patterns natively:

- the existing pending-write queue becomes a recoverable at-least-once spool;
- sessions gain optional idempotency keys;
- typed project-scoped handoffs become part of the Dialogue Layer;
- observation retrieval fuses semantic and lexical channels using deterministic RRF;
- authority remains a bounded, explainable ranking hint;
- all schema and MCP changes are additive;
- canonical second-brain memory remains outside Axon's write authority.

## Alternatives Considered

### Option A — Run ai-memory as a sidecar

- **Pros:** Faster reuse of an existing implementation and independent release cadence.
- **Cons:** Duplicates storage, capture, session, retrieval, and operational ownership.
- **Rejected because:** The project owner explicitly selected native integration and Axon already
  contains the required extension points.

### Option B — Replace Axon memory with ai-memory

- **Pros:** Adopts the upstream feature set wholesale.
- **Cons:** Breaks existing MCP contracts, duplicates organizational memory infrastructure, and
  creates a high-risk migration.
- **Rejected because:** It violates compatibility and architecture-preservation requirements.

### Option C — Do nothing

- **Pros:** No implementation or migration risk.
- **Cons:** A claimed queue can lose events on failure; sessions lack replay keys; handoffs are
  untyped; semantic-only observation ranking misses exact identifiers.
- **Rejected because:** The observed reliability and retrieval gaps remain.

## Consequences

### Positive

- Capture survives process and indexing failures without external infrastructure.
- Agent continuity becomes explicit and queryable.
- Retrieval handles semantic and exact-term signals with explainable ordering.
- Existing clients and databases upgrade additively.

### Negative / Trade-offs

- DuckDB gains more DB-only operational state that cannot be rebuilt from source files.
- The MCP surface and Dialogue Layer become larger.
- At-least-once replay may repeat indexing work.
- RRF introduces additional bounded queries per memory search.

### Risks & Mitigations

- **Risk:** invalid handoff transitions corrupt continuity. **Mitigation:** centralized state machine
  and integration tests.
- **Risk:** arbitrary authority manipulates results. **Mitigation:** clamp to `[0.5, 2.0]`, expose
  evidence, and never use it for authorization.
- **Risk:** schema upgrade behaves differently on old DuckDB files. **Mitigation:** additive nullable
  migration columns, `COALESCE`, and old-database integration tests.
- **Risk:** queue replay duplicates work. **Mitigation:** path deduplication and idempotent indexing.
- **Risk:** regression in published clients. **Mitigation:** optional fields/new tools only and
  multi-platform CI before merge.

## Affected Standards

- Enterprise Constitution §§2.2–2.5, 5, 7, 10 and 14.
- Memory Architecture Standard — local-first, deterministic retrieval, canonical memory boundary.
- Resilience Patterns Standard — retry, idempotency, bounded failure handling.
- Data Contracts & Schema Evolution Standard — additive compatibility.
- Advanced Testing Strategy Standard — unit, integration, smoke and evaluation evidence.
- Observability Playbook — structured operational evidence without sensitive content.
- C++ Architecture, Build & Toolchain, NFR and Testing Standards.

This ADR introduces no exception or standards deviation.
