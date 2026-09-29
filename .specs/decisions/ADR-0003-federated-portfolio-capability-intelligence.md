# ADR-0003 — Federated Portfolio Capability Intelligence

## Status

Accepted

Accepted by the responsible owner on 2026-08-30 after independent G1 verification. This acceptance
authorizes additive local implementation under the governed delivery graph; it does not authorize
schema/data migration of shared environments, secrets, infrastructure changes, external capability-
graph writes, merge, deployment, tag or release.

## Context

Axon currently stores observed code intelligence in one project-local DuckDB and can aggregate
registered repositories live. It has no transactional change journal, durable central projection,
capability signature, observed/declarative comparison or production portfolio UI. The existing
`group_impact` implementation also opens secondary databases inconsistently and queries obsolete
edge columns.

The organization already operates governed shared PostgreSQL/pgvector, Qdrant and FalkorDB. The
Platform Capability Graph is Git-authoritative; generated databases are projections. MemPalace and
Graphify provide useful precedents for provider conformance, target isolation, replay, global graph
identity and exploration, but neither is a suitable Axon runtime dependency.

## Decision

1. Preserve every project `.axon/index.duckdb` as the relational authority for that repository's
   observed code state.
2. Add a transactional append-only change journal/outbox in the same DuckDB transaction as index
   mutations. This is not Event Sourcing: the event log is not the project write model's source of
   truth and project state is not reconstructed exclusively from events.
3. Materialize independent local and shared CQRS read projections. Local uses portfolio DuckDB;
   shared uses PostgreSQL/pgvector and is privately owned by Axon Server.
4. Treat Qdrant semantic retrieval and FalkorDB graph traversal as optional, rebuildable read
   models. PostgreSQL is the durable central projection from which those models are repaired.
5. Select providers by explicit role (`portfolio_store`, `semantic_index`, `graph_projection`) and
   named profile. Do not abstract the existing project database behind a generic backend.
6. Move data between authorities and projections through read-only local pulls or authenticated,
   bounded remote event/snapshot submission. Clients never mount a shared DuckDB or receive direct
   central database credentials.
7. Distinguish logical `repository_id` from the physical index's `index_stream_id`. Clones/worktrees
   may share the former but have independent sequence/cursor streams; profiles select one default
   variant. Reconcile each stream by sequence, epoch and manifest. Notifications are hints only.
   All applies and rebuilds are idempotent and repository/stream-partitioned.
8. Store versioned capability signatures, evidence references and provenance centrally, never a
   second full source copy.
9. Generate duplicate/convergence candidates through multiple independent deterministic signals and
   RRF; name or embedding alone is insufficient.
10. Keep observed capabilities, Git-declared capabilities, matches and drift separate. All
    governance-changing actions remain human-gated.
11. Extend Axon Web with bounded server-side graph/query APIs and an offline-capable portfolio UI.
    The browser and graph database never become authoritative.
12. Consume existing governed shared infrastructure; do not create project-local PostgreSQL,
    Qdrant or FalkorDB services.
13. Require authentication for all new portfolio HTTP endpoints. Shared traffic uses TLS. Preserve
    existing plaintext loopback developer operation only as a narrow, process-local exception to
    SI-25, with loopback-only bind and ephemeral credentials; no non-loopback plaintext is allowed.
14. Use hermetic real-provider instances for integration tests. Shared development services are
    exercised only by a separately authorized live compatibility smoke, never as mutable shared
    integration-test fixtures.
15. Bind every remote index stream to an authenticated publisher, server-issued binding and opaque
    root/contract fingerprints. Quarantine a duplicated stream binding or divergent logical
    identity, not legitimate distinct clone/worktree streams. Reidentify a logical repository only
    through an owner-approved handoff while the physical stream cursor/sequence remains continuous.
16. Transfer repository snapshots as bounded, idempotent generation/session chunks. Stage and
    verify all chunks before atomically activating a partition; retain the prior generation until
    activation succeeds.

## Alternatives Considered

### One shared DuckDB over a network mount

Rejected. DuckDB is embedded and a network-shared file would create locking/corruption and lifecycle
risks. A single owning server may use embedded DuckDB, but clients communicate through APIs.

### PostgreSQL as replacement for all local project databases

Rejected. It would break local-first operation, repository independence and existing contracts.

### Choose exactly one of PostgreSQL, Qdrant or FalkorDB

Rejected. They serve complementary relational, semantic and graph roles. Treating them as
interchangeable creates ambiguous authority and leaky abstractions.

### Use Graphify or MemPalace as a sidecar/runtime dependency

Rejected. Their Python runtimes duplicate Axon's index/retrieval responsibilities. Adopt patterns
and conformance behavior through native C++ ports/adapters instead.

### Full Event Sourcing

Rejected. Project current state remains authoritative and can be reconciled by snapshot/manifest.
The journal is a transactional outbox/change feed, avoiding the aggregate replay/upcaster burden of
an event-sourced write model.

### Central PostgreSQL only, no local portfolio projection

Rejected. It would make portfolio intelligence unavailable offline and violate fail-soft local-first
operation.

## Consequences

### Positive

- Independent local authority and shared portfolio intelligence coexist.
- Central concurrency and multi-user serving use a suitable relational server.
- Semantic and graph workloads receive specialized, disposable read models.
- Lost notifications and partial provider failures converge through deterministic reconciliation.
- Results carry provenance, freshness and explanations suitable for human governance.

### Negative

- Four storage roles increase operational and test surface.
- Eventual consistency and cross-store generation tracking become explicit product concerns.
- PostgreSQL/Qdrant/FalkorDB adapters require real integration environments and schema lifecycle.
- Axon must establish stronger layering than its current compact source layout.

## Mitigations

- Keep project persistence unchanged; introduce narrow application ports only for new derived roles.
- Commit PostgreSQL first and update read models through a durable projection outbox.
- Tag every read-model row/document/node with repository id, index stream id, source epoch and
  projection generation.
- Provide conformance suites, reference/in-memory behavior and provider-disabled tests.
- Bound request, candidate and subgraph sizes; validate roots, identifiers, filters and thresholds.
- Bound staged snapshot bytes/sessions per principal, expire incomplete generations, validate every
  chunk digest/count/index and keep the previous generation live until atomic activation.
- Require auth for all new endpoints; use ephemeral local credentials and TLS for shared mode; never
  log source or credentials. Match token principal to the stored repository binding and treat
  client-supplied identity/approval fields as assertions requiring server-side records.
- Roll out in shadow mode and compare incremental, rebuild and live-aggregation results before
  selecting the shared profile as environment default.

## Rollout and Rollback

Roll out additively: baseline repair; schema ledger/journal flag; shadow local projection;
PostgreSQL projection; Qdrant/FalkorDB shadow read models; capability intelligence; read-only UI;
then explicit profile promotion. Roll back by stopping projectors/server, routing queries to current
local aggregation and deleting/rebuilding derived stores. Additive local journal tables remain
harmless to older binaries. No rollback changes `v1.2.16`.

## References

- `.specs/features/portfolio-capability-intelligence/spec.md`
- `.specs/features/portfolio-capability-intelligence/design.md`
- `.specs/features/portfolio-capability-intelligence/tasks.md`
- `docs/evidence/mempalace-portfolio-capability-intelligence-study-2026-08-30.md`
- Enterprise Constitution §§2, 5, 7, 9, 10 and 14
- CQRS, Hexagonal Architecture, Data Contracts, Resilience, Security, Observability, Advanced
  Testing and C++ standards
- Platform Capability Intake v2
