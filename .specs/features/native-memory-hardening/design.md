# Native Memory Hardening — Design

## Bounded Context and Ownership

Axon owns project-local code context and Dialogue Layer memory. The implementation extends those
existing boundaries. The second-brain remains the canonical organizational knowledge system and is
not written by this feature.

## Architecture Approach

- Extend the existing single-process DuckDB model; do not introduce a second service.
- Treat the pending-write file as an at-least-once spool. A `.processing` claim is the durable
  receipt and is deleted only after successful indexing.
- Model handoffs as a Dialogue Layer aggregate with explicit transitions.
- Keep retrieval deterministic: semantic candidates and lexical candidates are fused by RRF with
  `k=60`; the fused score is multiplied by bounded authority.
- Keep MCP and HTTP changes additive. Existing callers require no migration.

This follows the Enterprise Constitution §§2.2, 2.3, 5, 7 and 10; the Memory Architecture,
Resilience, Data Contracts, Advanced Testing, Observability and C++ standards; and ADR-0001.

## Data Model

Additive DuckDB changes:

```sql
ALTER TABLE observations ADD COLUMN authority DOUBLE DEFAULT 1.0;
ALTER TABLE sessions ADD COLUMN idempotency_key VARCHAR;

CREATE UNIQUE INDEX idx_sessions_idempotency
  ON sessions(thread_id, idempotency_key);

CREATE TABLE handoffs (
  id BIGINT PRIMARY KEY,
  source_session_id BIGINT,
  target_agent VARCHAR NOT NULL,
  project_root VARCHAR NOT NULL,
  working_directory VARCHAR NOT NULL,
  objective VARCHAR NOT NULL,
  context VARCHAR NOT NULL DEFAULT '',
  status VARCHAR NOT NULL DEFAULT 'pending',
  claimed_by VARCHAR,
  result VARCHAR,
  idempotency_key VARCHAR,
  created_at TIMESTAMP NOT NULL DEFAULT now(),
  claimed_at TIMESTAMP,
  completed_at TIMESTAMP
);
```

DuckDB cannot add constrained columns to old tables, so upgrade columns remain nullable and reads
use `COALESCE`. Fresh schemas use defaults and application validation enforces invariants.

## Handoff Contract

States: `pending`, `claimed`, `completed`, `cancelled`.

- create: produces `pending`; same source session and idempotency key returns the existing handoff.
- claim: `pending → claimed`; replay by the same claimant is idempotent.
- complete: `claimed → completed`; only the current claimant can complete.
- cancel: `pending|claimed → cancelled`; terminal states reject further transitions.
- get/list: read-only, with optional `status` and `target_agent` filters.

The MCP boundary validates enums, maximum lengths, and confines `working_directory` to the project
root using canonical filesystem paths.

## Retrieval Contract

1. Embed the query and retrieve a bounded semantic candidate set.
2. Normalize at most eight lexical query terms and retrieve a bounded lexical candidate set.
3. Assign each observation `1/(60 + rank)` per channel in which it appears.
4. Sum channel contributions.
5. Multiply by `clamp(authority, 0.5, 2.0)`.
6. Sort by final score descending, then observation id ascending.

An observation may appear in only one channel. Ranking evidence is returned to callers.

## Capture Recovery

- If `.axon/pending-writes.processing` exists, process it before claiming a new queue.
- Do not remove the claim before `index_files` succeeds.
- On a handled failure, merge the claimed paths back into the queue, retaining the claim until the
  merge is durable. A crash may create duplicates, which are harmless by FR-03.
- Emit structured stderr telemetry containing counts and error class, never source content.

## Security and Privacy

- No handoff is allowed to target a working directory outside `project_root`.
- Content is stored locally in the existing project database.
- Tool inputs use bounded lengths and enum validation.
- Authority is ranking metadata only and never grants permission.
- No secrets, turn content, observation content, or paths are included in telemetry events.

## Observability

- `queue_drain_recovered`, `queue_drain_failed`, and `queue_drain_completed` structured events.
- `handoff_created`, `handoff_claimed`, `handoff_completed`, and `handoff_cancelled` events through
  the existing telemetry layer where possible.
- Search results expose ranking evidence; eval tooling records recall and latency locally.

## Rollout and Rollback

- Rollout is an additive minor release after multi-platform CI.
- Rollback is binary-only: older Axon versions ignore additive columns/tables.
- No destructive migration is performed.
- Failed queue batches remain recoverable on disk.

## Alternatives Considered

- Sidecar ai-memory process: rejected by explicit owner decision and because it duplicates local
  persistence and lifecycle ownership.
- External broker/outbox: rejected for a local single-user process; violates local-first and adds
  operational dependency.
- Semantic-only retrieval: retained as a channel but insufficient for exact identifiers.
- LLM reranking: rejected from the hot path due to nondeterminism, cost, and offline requirements.
