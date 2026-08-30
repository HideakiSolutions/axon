# Portfolio Capability Intelligence — Feature Specification

## Purpose

Evolve Axon from project-scoped indexing into a local-first federated architecture-intelligence
engine. Every project keeps an independent authoritative DuckDB index while local and shared
portfolio projections derive capability, dependency, contract, consumer, duplication and drift
intelligence without taking authority from code or the Git capability graph.

This specification is governed by ADR-0003 and the consolidated evidence study in
`docs/evidence/mempalace-portfolio-capability-intelligence-study-2026-08-30.md`.

## Authority Boundaries

- Project `.axon/index.duckdb` is authoritative for observed code state in that repository.
- Repository-owned Git capability graph fragments are authoritative for declared capabilities and
  ownership.
- PostgreSQL, portfolio DuckDB, Qdrant and FalkorDB contain derived, rebuildable projections.
- Semantic or structural discovery never creates ownership, adoption or refactoring authority.
- Axon never edits source repositories, Git fragments, vault memory, packages or ADR status as a
  consequence of discovery.

## Actors

- Developers and agents querying one project or a portfolio.
- Portfolio architects reviewing duplicate, convergent and shared-primitive candidates.
- Capability owners reviewing observed/declarative drift.
- Operators monitoring sync, lag, rebuilds and degraded providers.
- Existing CLI, MCP and HTTP clients whose contracts must remain compatible.

## Functional Requirements

### Local authority and journal

- **FR-001:** Every project MUST retain its independent `.axon/index.duckdb`.
- **FR-002:** The local index MUST expose an additive schema ledger.
- **FR-003:** Every committed observable index mutation MUST atomically append a versioned journal
  event in the same DuckDB transaction.
- **FR-004:** Every physical project index MUST persist a stable `index_stream_id`; journal sequence
  MUST be strictly monotonic per stream. `repository_id` remains the logical source-repository id,
  so legitimate clones/worktrees share it without sharing cursors or sequences.
- **FR-054:** Repository identity MUST come from `repository-contract.yaml` when present; legacy
  projects MUST receive one persisted registry identity. Each physical index has a separate
  `index_stream_id`; profiles select one default stream per logical repository and expose other
  clone/worktree/branch streams only as explicit variants. Duplicated stream ids, divergent logical
  identity claims, later contract adoption and identity changes MUST quarantine ambiguity and
  require an explicit, auditable
  `RepositoryReidentified` old/new mapping. Remote publication MUST bind the repository identity to
  an authenticated publisher and opaque root/contract fingerprints; reidentification MUST close the
  old stream/partition, hand off its sequence/cursor, activate an approved new-id snapshot and then
  resume the new journal tail without sequence reuse.
- **FR-005:** Event identity MUST be unique and replay-safe; `(index_stream_id, sequence)` is the
  mandatory idempotency key.
- **FR-006:** Events MUST include schema version, repository id, index stream id, sequence, event
  id/type, epoch,
  previous epoch where applicable, commit/ref when available, affected references, timestamp and
  snapshot manifest hash where applicable.
- **FR-007:** Minimum facts are `IndexSnapshotCompleted`, `IndexFilesUpdated`,
  `IndexFilesDeleted`, `IndexSymbolsUpdated`, `IndexContractsUpdated`, `IndexRoutesUpdated`,
  `RepositoryReidentified` and `RepositoryRemoved`.
- **FR-008:** Deletes MUST survive downstream processing as tombstones.
- **FR-009:** A deterministic repository manifest and epoch MUST represent the indexed state.
- **FR-010:** The journal is a transactional outbox/change journal, not the source of truth for the
  project write model; the current local index remains authoritative.

### Projection and reconciliation

- **FR-011:** Axon MUST support a local portfolio DuckDB projection under `AXON_REGISTRY_DIR`.
- **FR-012:** Axon MUST support a shared PostgreSQL portfolio projection owned by Axon Server.
- **FR-013:** Local and shared projections MUST coexist with independent cursors.
- **FR-014:** Project databases opened by a projector MUST always be `READ_ONLY`.
- **FR-015:** Projection apply MUST be atomic per repository/stream-variant batch and MUST NOT
  advance its cursor
  on partial failure.
- **FR-016:** Duplicate replay MUST be idempotent.
- **FR-017:** Notification by spool/marker or localhost MAY reduce latency, but convergence MUST rely
  on polling plus epoch/manifest reconciliation.
- **FR-018:** Axon MUST support selective repository rebuild and deterministic full rebuild.
- **FR-019:** Full rebuild MUST be semantically equivalent to uninterrupted incremental projection.
- **FR-020:** Temporary repository/provider unavailability MUST produce explicit stale/degraded
  state; it MUST NOT silently delete projected data.
- **FR-021:** Remote clients MUST submit bounded event/snapshot batches through authenticated Axon
  APIs and MUST NOT receive direct central database credentials. Each remote batch/snapshot MUST be
  self-contained in projected metadata, signatures, evidence references and tombstones so Axon
  Server never needs filesystem access to the client's DuckDB.

### Role-aware providers

- **FR-022:** Provider selection MUST be by role, not by a single interchangeable backend switch.
- **FR-023:** DuckDB MUST remain the required local/default provider.
- **FR-024:** PostgreSQL MUST implement the durable shared portfolio-store role.
- **FR-025:** Qdrant MAY implement the semantic-index role and MUST remain rebuildable.
- **FR-026:** FalkorDB MAY implement the graph-projection role and MUST remain rebuildable.
- **FR-027:** pgvector MUST provide an exact relational baseline/fallback for semantic retrieval.
- **FR-028:** Every provider MUST advertise capabilities, health, schema/protocol version and
  maintenance operations; unsupported requirements fail explicitly.
- **FR-029:** A provider target marker MUST bind server instance, namespace and protocol version and
  reject accidental target drift.
- **FR-030:** The governed Hideaki profile MUST consume existing shared services rather than create
  project-local database containers.

### Capability intelligence

- **FR-031:** Axon MUST extract versioned capability signatures incrementally and deterministically.
- **FR-032:** Signatures MUST support repository/context/module/path/name/summary, public symbols,
  routes, handlers, events, schemas, DTOs, dependencies, graph neighborhood, tests, technologies,
  normalized AST fingerprints, optional embeddings, evidence/provenance and freshness.
- **FR-033:** Central stores MUST contain signatures and verifiable references, not complete source
  copies.
- **FR-034:** Candidate generation MUST combine independent semantic, name, contract, endpoint,
  event, structural, dependency, graph-neighborhood, test/behavior and domain/ownership signals.
- **FR-035:** Ranking fusion MUST be deterministic, bounded and explainable; RRF is the default.
- **FR-036:** No candidate may be classified solely by name or embedding.
- **FR-037:** Results MUST expose final score, per-signal score/rank, evidence, matching references,
  relevant differences, freshness, confidence, classification, recommendation and invalidators.
- **FR-038:** Classifications MUST include `exact_duplicate`, `convergent_capability`,
  `shared_primitive_candidate`, `local_specialization`, `semantic_coincidence` and
  `insufficient_evidence`.
- **FR-039:** A same-name/different-domain candidate MUST NOT be promoted as a duplicate.
- **FR-040:** Operation without embeddings MUST remain functional.

### Declared graph and governance

- **FR-041:** `observed_capabilities`, `declared_capabilities`, `capability_matches` and
  `capability_drift` MUST remain separate models.
- **FR-042:** Git declarations MUST be imported read-only with source commit and path provenance.
- **FR-043:** Axon MAY recommend intake, consolidation, library extraction, existing-contract
  adoption, keep-local or ownership review.
- **FR-044:** Axon MUST NOT write capability fragments, declare owners, publish packages, move code,
  accept ADRs or start cross-repository refactors automatically.

### Surfaces and UI

- **FR-045:** CLI MUST add portfolio profile/sync/reconcile/rebuild/status and capability
  list/search/duplicates/compare/consumers/drift operations without removing existing commands.
- **FR-046:** MCP MUST add equivalent bounded tools without removing or renaming existing tools or
  fields.
- **FR-047:** HTTP APIs MUST be versioned, authenticated for every new portfolio endpoint, bounded
  and paginated. Shared-server traffic MUST use TLS; local loopback transport follows the explicit
  ADR-0003 exception/mitigation and never binds externally without TLS.
- **FR-048:** Axon Web MUST visualize repository/context/capability/implementation topology,
  abstractions, consumers, packages, contracts, paths, duplicate comparisons, drift and freshness.
- **FR-049:** The UI MUST request bounded server-side subgraphs; it MUST NOT require a whole
  portfolio snapshot in the browser.
- **FR-050:** Qdrant/FalkorDB outage MUST leave the UI usable with explicit degraded-state evidence.
- **FR-051:** UI actions that propose governance changes MUST remain read-only proposal artifacts
  until a separate authorized goal.
- **FR-052:** An optional static HTML snapshot MAY be exported for offline evidence.
- **FR-053:** New HTTP endpoints MUST have a committed OpenAPI contract and provider/consumer
  compatibility verification; asynchronous/remote ingest MUST have versioned message fixtures.

## Non-Functional Requirements

- **NFR-001 Reliability:** zero confirmed local index mutations without a corresponding committed
  journal fact.
- **NFR-002 Idempotency:** duplicate events, notifications, snapshots and projection retries produce
  the same state.
- **NFR-003 Determinism:** signatures, fingerprints, manifests, RRF ties and rebuilds are reproducible.
- **NFR-004 Local-first:** indexing and local portfolio queries work without network, server, API key
  or external provider.
- **NFR-005 Fail-soft:** central/read-model outages never block local indexing and are visible.
- **NFR-006 Security:** canonical root validation, symlink confinement, bounded inputs, prepared
  statements, no arbitrary SQL, secret redaction and no source content in logs.
- **NFR-007 Privacy:** central metadata is allowlisted and excludes complete code, secrets and PII.
- **NFR-008 Compatibility:** schemas and public surfaces are additive and operationally reversible.
- **NFR-009 Portability:** Linux, macOS and Windows remain supported.
- **NFR-010 Performance:** candidate fan-out and graph expansion are bounded; only affected
  repository partitions are recomputed.
- **NFR-011 Observability:** structured events cover emit/apply/lag/cursor/epoch/stale/retry/rebuild,
  extraction/candidate counts and drift with limited cardinality.
- **NFR-012 Testability:** domain/application rules run without infrastructure; each adapter has a
  hermetic real-provider integration suite; E2E uses at least three repositories. Governed shared
  services receive a separate live compatibility smoke after explicit per-run authorization.
- **NFR-013 Evaluability:** a versioned corpus with explicit truth set reports precision@K,
  recall@K, false positives/negatives and name-only/semantic-only/multi-signal baselines.
- **NFR-014 Resource use:** builds and heavy suites run sequentially at maximum `-j2`; performance
  budgets are based on measured hardware/corpus baselines.
- **NFR-015 Accessibility:** portfolio UI supports keyboard operation, semantic labels, reduced
  motion and non-color confidence/state indicators.
- **NFR-016 Acceptance testing:** user-visible portfolio journeys have executable Given/When/Then
  scenarios; contract-only and live-validated provider evidence are reported separately.

## Constraints and Human Gates

- ADR-0003 MUST be explicitly accepted before production code implementing its decisions.
- Schema/data migration, security posture, infrastructure, secrets, deployment, merge, tag and
  release remain separately human-gated.
- Existing shared PostgreSQL/Qdrant/FalkorDB may be consumed only through governed provisioning;
  this feature does not create credentials or mutate shared infrastructure during design.
- The immutable tag `v1.2.16` MUST never move or change.
- Existing untracked files and other worktrees MUST remain untouched.

## Mandatory Acceptance Scenarios

- **AC-001 Journal atomicity:** crash before commit leaves neither index mutation nor event; crash
  after commit leaves both, with monotonic sequence and stable event id.
- **AC-002 Incremental replay:** duplicate batch and notification replay do not change semantic
  state or advance a cursor twice.
- **AC-003 Lost notification:** polling plus epoch/manifest reconciliation converges after a marker
  is discarded.
- **AC-004 Rebuild equivalence:** selective and full rebuild produce the same normalized projection
  as uninterrupted incremental ingestion.
- **AC-005 Read-only secondaries:** projector, reconcile and `group_impact` leave every secondary
  DuckDB byte-identical.
- **AC-006 Repository lifecycle:** removal creates tombstones; temporary absence is stale; return
  and owner-approved reidentification converge without silent deletion or collision. A mismatched
  principal/binding/fingerprint for the same stream is quarantined, legitimate main/worktree streams
  coexist under one logical repository with one configured default, and the old logical partition
  remains auditable as superseded after the new snapshot generation activates.
- **AC-007 Schema evolution:** an older valid local/projection database upgrades through additive
  migrations and remains readable by the compatible rollback path.
- **AC-008 Remote self-containment:** a server with no client filesystem access ingests a bounded
  event/delta batch, repairs from snapshot plus tail and resumes after timeout by cursor probe.
- **AC-009 Three-repository truth set:** synthetic repositories contain one real duplicate, one
  convergent capability, one legitimate specialization and one semantic coincidence.
- **AC-010 Declared/observed drift:** the truth set contains one declaration without observable
  implementation and one observation without declaration; ownership is never inferred.
- **AC-011 Explainability:** CLI, MCP and HTTP return final/per-signal scores, evidence, differences,
  freshness, confidence, classification, recommendation and invalidators for the truth set.
- **AC-012 Provider degradation:** no-embedding mode works; Qdrant and FalkorDB outages retain
  relational/UI functionality with explicit degraded status.
- **AC-013 Security and bounds:** invalid roots/symlink escapes, identifiers, SQL/Cypher-like input,
  filters, thresholds, pagination, batch size and graph fan-out are rejected without data leakage.
- **AC-014 UI journey:** keyboard-only user can inspect topology, implementation/abstraction chains,
  package consumers, candidate comparison and drift without loading the whole graph or using a CDN.
- **AC-015 Evaluation:** versioned truth set reports reproducible precision@K/recall@K and false
  positives/negatives for name-only, semantic-only and multi-signal engines.
- **AC-016 Compatibility:** all existing CLI/MCP/HTTP schemas and behavior remain present and the
  full preexisting suite has no regression on supported operating systems.

## Definition of Done

All FR/NFR above have direct automated or artifact evidence; local indices remain independent;
incremental and rebuilt projections are equivalent; all providers pass conformance and degraded-mode
tests; candidates are multi-signal and explainable; declared/observed authority is preserved; CLI,
MCP, HTTP and UI work end-to-end; full quality gates and independent verification pass; worktree is
clean and delivery is PR-ready. Merge, deployment and release remain outside completion unless
separately authorized.
