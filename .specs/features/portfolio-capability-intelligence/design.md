# Portfolio Capability Intelligence — Design

## 1. Context Map

```text
Project Index Context              Portfolio Intelligence Context
---------------------              ------------------------------
.axon/index.duckdb                 local portfolio.duckdb
  files/symbols/routes/edges  ---> or shared PostgreSQL
  transactional journal              | durable projection outbox
  epoch + manifest                   +--> Qdrant semantic read model
                                      +--> FalkorDB graph read model
Git Capability Graph --------RO----> declared capability projection
                                      |
                                      +--> CLI / MCP / HTTP / Axon Web
```

The project context owns observed code facts. The Git graph owns declarations and ownership. The
portfolio context owns no canonical business fact; it joins derived evidence for query and review.

## 2. Layering

New code follows ports and adapters without rewriting existing project persistence:

```text
src/portfolio/domain/          pure ids, events, signatures, scores, classifications
src/portfolio/application/     sync/reconcile/rebuild/extract/search/compare use cases and ports
src/portfolio/infrastructure/  DuckDB/PostgreSQL/Qdrant/FalkorDB/Git/HTTP adapters
src/portfolio/delivery/        CLI, MCP, HTTP DTO translation
```

Existing `src/core/db.*` remains the project-index adapter. Composition occurs in `main`/server
construction. Domain and application headers expose no DuckDB, libpq, HTTP or JSON-library types.

## 3. Identity and Versioning

- `repository_id`: logical source-repository UUID from `repository-contract.yaml` when present. A
  legacy repository receives a persisted registry UUID. Legitimate clones and worktrees share this
  id; it groups variants but does not identify a database/cursor.
- `index_stream_id`: UUID generated and persisted in each physical `.axon/index.duckdb`. It
  identifies one authoritative index publication stream. A copied DB/duplicated stream id with a
  different publisher binding is quarantined; a new clone/worktree DB gets a new stream id.
- `sequence`: unsigned monotonic counter scoped to `index_stream_id`.
- `event_id`: deterministic digest over repository id, stream id, sequence, type and epoch.
- `index_epoch`: deterministic digest of schema version, repository id, commit/ref when known and
  manifest root.
- `manifest_hash`: BLAKE3 Merkle-style digest over sorted `(path, content_hash, entity_digest)` rows.
- `projection_generation`: UUID/digest created for a full/repository rebuild and attached to all
  Qdrant/FalkorDB records.
- All wire/data schemas carry explicit string schema versions and additive evolution rules.

### Identity binding, collision and reidentification

Local registry validation permits one logical `repository_id` at multiple canonical roots only when
each root points at a distinct `index_stream_id`. Registry/profile configuration selects exactly one
default stream per logical repository; additional clone/worktree/branch streams are explicit
variants and are never silently merged into the default portfolio view. Two roots claiming the same
stream id, or divergent repository-contract identity digests for one logical id, are quarantined.

A remote server cannot and does not compare client paths. During governed stream registration it
stores a binding of authenticated `principal_id`, server-issued `binding_id`, `repository_id`,
`index_stream_id`, `identity_epoch`, keyed HMAC `root_fingerprint` and optional
`repository_contract_digest`. The HMAC key is process/server configuration; raw roots are never
sent. Every batch/snapshot repeats this descriptor and the authenticated principal MUST match the
stored binding. A changed fingerprint/principal for the same stream is quarantined; a different
stream under the same logical repository is a valid explicit variant. This detects copied index
streams without treating legitimate clones/worktrees as repository collisions.

Reidentification is an explicit two-stream handoff:

1. owner approval creates a server/local grant bound to old/new repository and binding ids;
2. the affected index stream emits `RepositoryReidentified` under the old logical repository as its
   final old-id event at sequence `N`, with
   `repository_id=old_repository_id`, `handoff_sequence=N` and the approval reference;
3. local commit atomically persists the old-id event, rebinds the same `index_stream_id` to the new
   logical repository and continues that stream at `N+1` (never resets/reuses a sequence);
4. server validates the grant and atomically advances the stream's old logical partition cursor to
   `N`, tombstones/closes that variant partition, records `superseded_by`, and creates a pending new
   logical partition for the same stream expecting `N+1`;
5. client publishes a generation-scoped new-id snapshot with `previous_identity` and through
   sequence at least `N+1`, then publishes its journal tail; only complete snapshot activation makes
   the new partition queryable.

Unapproved identity changes, old/new/top-level mismatches, reused sequences, principals or bindings
fail closed. Temporary missing repositories remain stale and never enter this handoff.

## 4. Local Transactional Journal

Additive DuckDB tables:

```sql
CREATE TABLE IF NOT EXISTS schema_migrations (
  component VARCHAR NOT NULL,
  version INTEGER NOT NULL,
  applied_at TIMESTAMP NOT NULL DEFAULT now(),
  checksum VARCHAR NOT NULL,
  PRIMARY KEY(component, version)
);

CREATE SEQUENCE IF NOT EXISTS index_event_sequence START 1;

CREATE TABLE IF NOT EXISTS index_events (
  repository_id VARCHAR NOT NULL,
  index_stream_id VARCHAR NOT NULL,
  sequence UBIGINT NOT NULL DEFAULT nextval('index_event_sequence'),
  event_id VARCHAR NOT NULL,
  schema_version VARCHAR NOT NULL,
  event_type VARCHAR NOT NULL,
  index_epoch VARCHAR NOT NULL,
  previous_epoch VARCHAR,
  source_ref VARCHAR,
  occurred_at TIMESTAMP NOT NULL DEFAULT now(),
  manifest_hash VARCHAR,
  payload_json VARCHAR NOT NULL,
  PRIMARY KEY(index_stream_id, sequence),
  UNIQUE(event_id)
);

CREATE TABLE IF NOT EXISTS index_tombstones (
  repository_id VARCHAR NOT NULL,
  index_stream_id VARCHAR NOT NULL,
  entity_kind VARCHAR NOT NULL,
  entity_key VARCHAR NOT NULL,
  deleted_sequence UBIGINT NOT NULL,
  deleted_epoch VARCHAR NOT NULL,
  deleted_at TIMESTAMP NOT NULL,
  PRIMARY KEY(index_stream_id, entity_kind, entity_key)
);
```

The indexer computes the mutation set, begins one DuckDB transaction, changes index rows, writes
tombstones and one or more journal facts, then commits. A failure rolls back all of them. Payloads
contain identifiers/digests and bounded metadata, never complete source.

## 5. Portfolio Store Port

```text
PortfolioStore
  capabilities() -> ProviderCapabilities
  health() -> ProviderHealth
  schema_version() -> SchemaVersion
  apply(repository_id, index_stream_id, expected_cursor, event_batch) -> ApplyResult
  replace_repository_stream(snapshot, expected_cursor) -> ReplaceResult
  stream_state(repository_id, index_stream_id) -> CursorEpochManifest
  list_capabilities(query_page) -> Page<CapabilitySummary>
  get_capability(id) -> CapabilityDetail
  list_candidates(query_page) -> Page<CandidateSummary>
  get_candidate(id) -> CandidateComparison
  list_drift(query_page) -> Page<DriftRecord>
  maintenance(kind) -> MaintenanceResult
```

Adapters implement the same behavioral conformance suite. The interface is role-specific and does
not attempt to abstract arbitrary SQL or Axon's current project database.

## 6. Storage Profiles

Registry v2 retains all v1 fields and adds named profiles:

```json
{
  "storage_profiles": {
    "local": {
      "transport": "local",
      "portfolio_store": {"provider": "duckdb", "path": "${AXON_REGISTRY_DIR}/portfolio.duckdb"},
      "default": true
    },
    "shared-dev": {
      "transport": "axon_http",
      "endpoint": "http://127.0.0.1:7071",
      "namespace": "axon",
      "portfolio_store": {"provider": "postgresql"},
      "semantic_index": {"provider": "qdrant"},
      "graph_projection": {"provider": "falkordb"},
      "default": false
    }
  }
}
```

Endpoints are non-secret. Credentials are referenced by environment/secret-provider keys and never
serialized. Exactly one default per role/profile class is allowed. Target markers bind instance id,
namespace and protocol version.

## 7. Projection Protocol

### Local pull

1. Resolve registered canonical root, logical repository id, persisted index stream id and whether
   this stream is the profile's default variant.
2. Open secondary DuckDB with `READ_ONLY` and no-follow/canonical-root checks.
3. Read events after that stream's central cursor in bounded batches.
4. Apply the stream-variant batch and cursor in one portfolio-store transaction.
5. Compare stream epoch/manifest; schedule stream snapshot replacement on mismatch.

### Remote push

1. Client commits local mutation without waiting for network.
2. Spool marker schedules sync.
3. `RemoteSyncAssembler` opens the committed local index read-only and joins each journal event to
   the current affected signatures/evidence plus tombstones. The journal remains compact; the
   transport is self-contained.
4. Client submits `axon/event-batch/v1`: target marker, authenticated publisher binding,
   repository identity, base cursor,
   idempotency key and ordered entries pairing each `axon/index-event/v1` fact with exactly one
   `axon/projection-delta/v1` at the same source sequence. Batches are capped at 500 events and split at
   entity/payload byte budgets; no source body is included.
5. Server validates auth, namespace, sizes, exact event/delta repository and sequence alignment,
   continuity and target marker, then commits the PostgreSQL partition and cursor atomically.
6. Bootstrap or repair creates one snapshot session/generation and submits
   `axon/repository-snapshot/v1` chunks. Each chunk carries session, generation, idempotency key,
   zero-based index, total count and digest; arrays are bounded to 10,000 records per kind per chunk.
   The server stages chunks idempotently, rejects conflicting duplicates, verifies every index,
   per-chunk digest, common binding/epoch/manifest/through-sequence and final partition manifest,
   then atomically activates the generation only when all `chunk_count` chunks exist. Missing chunks
   are listed for resume and leave the previous partition active. There is no repository-size cap;
   large partitions use more bounded chunks. After activation, ingestion resumes with the journal
   tail at `through_sequence + 1`. The server never needs client paths.
7. Ambiguous timeout is resolved by querying cursor before retry. Batch idempotency and
   `(index_stream_id, sequence)` make resend safe; resumable snapshot chunks use the same target,
   manifest and generation identity.

The wire contracts are `schemas/event-batch-v1.schema.json`,
`schemas/projection-delta-v1.schema.json` and `schemas/repository-snapshot-v1.schema.json`. Local
pull may assemble the same deltas in-process, but it does not serialize or copy complete source.

### Read-model fanout

PostgreSQL commits a durable `projection_outbox` row in the same transaction as its portfolio
state. Qdrant and FalkorDB workers apply it idempotently. Their checkpoints/generation remain in
PostgreSQL. Failed fanout never rolls back the durable projection and is surfaced as degraded lag.

## 8. PostgreSQL Model

Minimum schemas/tables include logical repositories, index streams/default-variant selection,
stream cursors, applied event ids, events retained for audit/rebuild, repository/stream variant
partitions, capability signatures/evidence, observed and declared
capabilities, matches/drift, candidates/signal scores/differences, tombstones, projection outbox and
provider health history. Migrations use expand-contract and a schema ledger. Deletes are logical
until every read-model generation has advanced beyond them.

PostgreSQL is still a derived portfolio projection: a full rebuild reads registered project
snapshots and Git declarations. It is durable because it coordinates concurrent server clients and
read models, not because it supersedes local/Git authority.

## 9. Capability Signature Pipeline

Deterministic extractors normalize names, public interfaces, routes/events/contracts, dependency
sets, bounded graph neighborhoods, associated tests, frameworks and AST structure. Evidence stores
`repository_id`, `index_stream_id`, local entity key, path, symbol/range, content/AST digest, extractor version and
epoch. Summaries are rule-based; embeddings are optional and carry model id, dimension, metric and
normalization.

Extraction is partition-incremental: journal payloads identify impacted files/entities; dependency
neighbors are recomputed only to a documented depth and cap. Repository snapshot rebuild is the
correctness fallback.

## 10. Candidate Generation and Classification

Independent channels return bounded ranked lists. RRF uses documented `k`, per-channel weights and
stable candidate-id tie breaking. Domain/ownership incompatibility is negative evidence. A
classifier maps evidence to the six required classes using deterministic thresholds first; any
future learned/LLM classifier is a separate optional objective and cannot override evidence gates.

Qdrant supplies semantic candidates when compatible. pgvector/exact cosine is the baseline and
fallback. Structural/name/contract/graph/test channels work with no embedding provider.

## 11. Git Declaration Import

Importers accept allowlisted registered roots and repository-owned fragment paths. They record
source repository, commit, path, schema version and parse diagnostics. Import is read-only. A
declaration missing implementation and an observation missing declaration are different drift
types. Ambiguous matches remain reviewable and never assign ownership.

## 12. HTTP, MCP and UI

Add versioned `/api/v1/portfolio/*` and `/api/v1/capabilities/*` endpoints with cursor pagination,
filter allowlists, response budgets and RFC-style typed errors while preserving current endpoints.
MCP tools mirror application queries/commands. Every new portfolio HTTP endpoint requires an
authenticated session, including loopback. Shared-server traffic requires TLS. Local browser mode
uses an ephemeral process-scoped credential and loopback-only bind; ADR-0003 records this narrow
plaintext-loopback exception to SI-25 rather than treating localhost as implicitly trusted.

Axon Web adds topology, capability detail, compare, consumers/package matrix, drift and status
views. Subgraph queries require roots, direction, depth, relation allowlist and node/edge limits.
Renderer assets are bundled; no CDN is required. Confidence, provenance and stale/degraded state
have text/icon encodings in addition to color.

## 13. Security and Threat Controls

- Canonicalize all repo roots and reject symlink escape.
- Validate UUIDs, namespaces, enums, cursor ranges, thresholds, page sizes and graph depth.
- Use prepared statements and fixed query templates; accept no raw SQL/Cypher/filter expression.
- Auth is mandatory for every new portfolio endpoint. TLS is terminated by a private proxy or native
  implementation for shared mode; plaintext is allowed only on loopback under the explicit ADR
  exception and process-scoped credential.
- Central allowlists metadata fields and redacts secrets/PII/source content from logs.
- Provider timeouts/retries are bounded; retry only idempotent operations.
- Qdrant/FalkorDB payloads carry no credential or complete source.

## 14. Observability

Structured events and bounded-cardinality metrics cover journal append, batch apply, lag/cursor,
epoch mismatch, stale repository, retries, rebuild lifecycle, extraction counts, candidate channel
counts, discard reasons, drift, read-model lag and UI truncation. Repository ids are hashed or
bounded labels where metrics cardinality requires it. Detailed per-repository status is query data,
not a metric label explosion.

## 15. Rollout and Rollback

Run local projector in shadow against current live aggregation, then PostgreSQL in shadow, then
Qdrant/FalkorDB. Compare manifests, counts and semantic result sets before enabling queries. UI is
read-only first. Rollback routes queries to current aggregation/local profile and stops projectors;
all central stores are rebuildable. Schema migrations remain additive until a separately approved
contract phase.

## 16. Test Environments

Unit tests use no I/O. Infrastructure adapter integration tests use hermetic real PostgreSQL,
Qdrant and FalkorDB instances provisioned per run (Testcontainers or the repository's portable
equivalent) with deterministic teardown, as required by AT-05/AT-06. They do not use shared mutable
development databases. A distinct governed-environment compatibility smoke may query and, only
with explicit per-run authorization, create/delete Axon-scoped synthetic test namespaces in the
shared services. Contract-only and live-validated states are reported separately.
