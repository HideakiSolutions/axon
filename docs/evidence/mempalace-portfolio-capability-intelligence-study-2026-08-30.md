# MemPalace absorption study and Portfolio Capability Intelligence proposal

**Date:** 2026-08-30

**Axon baseline:** `f776d73a95628e14328ed4b9f66d89318c437832` (`main`, v1.3.1 installed)

**MemPalace baseline:** `a9f345cc63254eb4dea7abad36963b85c9f8453a` (`develop`, package 3.8.0)

**Graphify baseline:** `680e3ed8edd3dc1fa1961050912941880b778207` (`develop`, package 0.9.52)

**Sources:** `/opt/references/mempalace`, `/opt/references/graphify` and the governed shared-infra
deployment under `/opt/hideakisolutions/shared-infra`

**Decision:** selective absorption (`partial`, high priority)

**Architectural decision status:** Proposed; owner gate is still required before implementation.

## 1. Executive conclusion

MemPalace should not become an Axon dependency, sidecar, or code base. It is a large Python memory
product while Axon is a compact C++ code-intelligence engine. Its value is architectural: it has
already exercised several boundaries that the proposed Portfolio Capability Intelligence needs:

- a versioned storage contract with typed results, explicit capabilities, health and maintenance;
- deterministic backend selection and mismatch protection;
- isolation by stable logical identity and optional server namespace;
- embedded and networked storage implementations behind the same contract;
- an authenticated HTTP MCP hub with a single writer owner;
- local/server write routing with fail-closed policy;
- append-only events, stable replica identity, origin sequences and idempotent replay;
- anti-entropy based on version vectors;
- separation between canonical facts and locally derived indexes;
- conformance suites, exact-vector reference behavior, repair, integrity and backup workflows;
- bounded hybrid retrieval and evidence-bearing ranking.

The strongest Axon design is not “make every project choose any database”. It is a role-based,
federated architecture in which different databases may coexist deliberately:

1. every project keeps its authoritative `.axon/index.duckdb`;
2. a local portfolio projection defaults to `${AXON_REGISTRY_DIR}/portfolio.duckdb`;
3. an optional shared Axon server owns its own central projection database and exposes it through
   authenticated MCP/HTTP; clients never mount or open that DuckDB file over the network;
4. local and shared projections can coexist, and query routing returns provenance and freshness;
5. a role-aware provider seam is introduced for derived stores only: DuckDB remains the universal
   local default, while PostgreSQL/pgvector, Qdrant and FalkorDB are optional, independently
   rebuildable server-mode projections;
6. the governed Hideaki environment enables those optional server providers in the first rollout,
   because they already exist as healthy shared infrastructure; Axon must not deploy duplicate
   database containers;
7. a native portfolio UI, informed by Graphify's graph explorer, visualizes implementations,
   abstractions, package consumption, call/contract relationships, duplicate candidates,
   ownership drift and freshness without making the browser or a graph database authoritative.

This preserves the original proposal's authority boundary and local-first behavior while adding
the requested multi-database and central-server capability without pretending that DuckDB is a
client/server database. The original prohibition on introducing PostgreSQL is superseded by the
owner clarification of 2026-08-30 only in this precise sense: existing governed shared services
may be consumed through optional adapters. The prohibition on adding a new sidecar, private
database stack or mandatory network dependency remains intact.

## 2. Evaluation method and limitations

The review covered repository structure, README, package/build metadata, recent history, RFCs,
storage backends, selection registry, local locks, HTTP hub, write routing, event log, replica/sync
code, deployment manifests, security guidance, conformance tests, repair tools and benchmarks.

Observed scale:

- 571 tracked files;
- approximately 157,994 Python lines including tests and tooling;
- 147 top-level `test_*.py` modules;
- 246 implementation/test Python files parsed successfully with Python AST;
- CI targets Python 3.9/3.11/3.13 on Linux and 3.13 on Windows/macOS, with an 80% CI coverage gate
  and an 85% configured coverage threshold;
- MIT license.

Graphify was reviewed separately as a UI/global-graph reference: 848 tracked files, approximately
151,094 Python lines, 238 top-level test modules and Apache-2.0 plus MIT/NOTICE licensing. A focused
run covering global graph, export and FalkorDB behavior produced 77 passes, one skip and three
dependency-only failures because `rapidfuzz` was not installed. No dependency was installed and the
repository-state guard restored the checkout cleanly.

A focused pytest run was attempted for backend, hub, proxy and routing tests. It did not start
because `chromadb` is not installed in the evaluation environment. Dependencies were intentionally
not installed. The repository-state guard removed the generated `tests/__pycache__/` and verified
a clean restoration. Claims below therefore come from file-backed implementation and test evidence,
not from a locally executed full suite.

Axon-first discovery was attempted with the installed Axon 1.3.1 binary. It was blocked by the
existing live DuckDB owner PID 2908784 from `axon-dist.v1.2.16.bak`. The process was preserved in
accordance with session continuity. Fallback was direct inspection of Axon code, schemas, tests and
documentation. This fallback was explicit, not silent.

## 3. What MemPalace actually provides

### 3.1 Storage provider contract

`mempalace/backends/base.py` defines two levels: a long-lived backend factory and a per-collection
interface. The contract includes:

- stable `PalaceRef.id` isolation;
- optional `namespace` isolation, advertised as a capability;
- typed query/get/lexical results;
- normalized “lower is closer” distance semantics;
- capability discovery instead of backend-name branching;
- backend and collection health;
- explicit maintenance kinds and observable maintenance outcomes;
- lifecycle methods for one logical database and the whole provider;
- specific, portable error types;
- stored embedding model identity and vector dimension compatibility.

This is a useful pattern for Axon, but the 710-line Python interface should not be translated
literally. Axon's seam must be smaller and role-specific.

### 3.2 Provider selection and mismatch protection

`mempalace/backends/registry.py` resolves a provider in this order:

1. explicit call/CLI value;
2. per-palace config;
3. environment;
4. artifact detection for migration only;
5. default (`chroma`).

Instances are cached, third-party providers can register through entry points, and local artifacts
are detected to prevent silently opening an existing store with the wrong provider. Server backends
also write local marker files containing target identity. Changing URL, namespace or remote target
then fails loudly.

Axon should absorb explicit precedence, mismatch detection, capability advertisement and the
conformance philosophy. It should not add a general runtime plugin ABI in the first iteration.

### 3.3 Multiple databases and server backends

MemPalace ships five providers:

| Provider | Topology | Relevant lesson |
| --- | --- | --- |
| Chroma | embedded local | zero-configuration default and local lifecycle |
| SQLite exact | embedded local | exact reference engine for correctness and conformance |
| Milvus | local Lite or remote server | one provider can expose topology-dependent capabilities |
| Qdrant | remote REST | remote target marker, namespaces and no driver dependency |
| PostgreSQL + pgvector | remote SQL | database/schema namespace and server concurrency |

The logical identity is independent of physical storage. Server-capable providers hash the logical
palace id into remote collection names and optionally prefix by namespace. Unsupported namespace
use fails rather than being ignored.

The limitation matters: MemPalace selects one provider for one palace operation. It is not itself a
federated query engine across heterogeneous providers. Axon must add federation and role-aware
routing rather than copy the provider selector and assume the problem is solved.

### 3.4 Shared central hub

MemPalace's team topology is:

```text
clients -- authenticated MCP/HTTP --> one MemPalace server --> networked vector store
```

The server:

- binds loopback by default;
- requires a bearer token on non-loopback binds unless an explicit insecure override is supplied;
- can terminate TLS or sit behind a trusted private reverse proxy;
- protects against Host/Origin abuse;
- supports read-only mode;
- serializes writes and owns the local writer lease;
- publishes authenticated `/statusz` and public minimal `/healthz`;
- records local server discovery metadata so local CLI writes can forward to the owner;
- refuses ambiguous fallback after a submission may have been accepted;
- supports Docker Compose with Qdrant and a hardened systemd unit.

This is directly relevant to a shared central Axon projection. The lesson is that the service, not
the database file, is the network boundary.

### 3.5 Local and central coexistence

MemPalace supports three useful modes:

- private local database;
- central shared hub;
- local replicas converging with peers.

Its replicated-palace RFC states the key separation: canonical facts/op-log are synchronized;
embeddings, vector graphs and other indexes are derived locally. The shipped logstream replication
uses stable replica ids, per-origin sequences, hybrid logical clocks, version vectors, artifact-first
transfer, hashes and idempotent fold. The full multi-master memory design is explicitly incomplete.

For Axon, only a subset is needed initially: stable logical repository identity, a distinct physical
index-stream identity, monotonic per-stream sequence, append-only local journal,
snapshot/manifest reconciliation and a single-writer derived
central projection. Multi-master project indexes are out of scope.

### 3.6 Retrieval and capability discovery lessons

MemPalace's search combines vector and lexical candidates, bounded candidate pools, metadata/date
filters and structural “closet” context. It declares the collection distance metric rather than
assuming cosine. Its benchmarks keep name/semantic baselines and inspect failure classes.

Axon already has a better fit for capability comparison: syntax trees, symbols, routes, dependency
edges, call relationships and tests. It should absorb the disciplined multi-signal evaluation and
capability-advertisement ideas, not MemPalace's memory-specific palace hierarchy.

### 3.7 Other capabilities worth selective absorption

| MemPalace capability | Axon disposition | Reason |
| --- | --- | --- |
| Storage conformance kit | Adopt pattern | Prevent DuckDB-specific assumptions from leaking into derived-store interfaces |
| Source adapter contract | Partial | Useful later for capability-graph importers; current RFC remains draft upstream |
| Provider health/maintenance | Adopt | Required for status, reconcile and operator-visible degradation |
| Exact reference backend | Adopt as test harness | In-memory/reference implementations can verify ranking and provider semantics |
| Embedding identity | Adopt | Axon must record model id, dimension and signature schema version |
| Server target marker | Adopt | Prevent client from pushing one repo to the wrong central portfolio |
| Writer routing policy | Adopt | `direct`, `prefer`, `require` maps well to local projector/server ownership |
| No fallback after ambiguous submission | Adopt | Prevent duplicate event batches after timeout |
| Logstream append-only envelope | Partial | Reuse origin/sequence/idempotency concepts, not task/patch domain fields |
| Anti-entropy version vectors | Partial/later | Cursor + epoch/manifest is sufficient for single-author repo journals in phase one |
| HLC conflict resolution | Defer | No multi-master mutation of project indexes is planned |
| Snapshot + tail bootstrap | Adopt | Correct pattern for new or long-offline central projections |
| Integrity/repair preflight | Adopt | Central projection requires verify, selective rebuild and deterministic full rebuild |
| Backup retention | Partial | Central is rebuildable; local event journals need explicit retention/compaction policy |
| Read-only tool mode | Adopt | Shared server should support query-only deployment |
| HTTP security defaults | Adopt | Required before any non-loopback Axon bind |
| SSE/long-poll coordination | Defer | Portfolio sync needs bounded batch endpoints; realtime UI can follow later |
| Memory “wings/rooms/drawers” | Reject for Axon | Domain mismatch; capabilities/repos/bounded contexts already provide the taxonomy |
| Verbatim code storage centrally | Reject | Violates the proposal's metadata/signature-only central boundary |
| Chroma/Milvus | Reject in phase one | Adds ungoverned providers without an environment-backed need |
| PostgreSQL/pgvector adapter | Adopt | Existing shared service fits durable concurrent central projection and exact metadata/vector joins |
| Qdrant adapter | Adopt as optional read model | Existing shared service fits bounded semantic candidate retrieval; it is never the system of record |
| FalkorDB graph projection | Adopt as optional read model | Existing shared service fits cross-repository traversal and the portfolio explorer; it remains rebuildable |
| Python entry-point plugins | Reject in phase one | Axon is a single C++ binary; ABI/version/security cost is not justified yet |
| LLM extraction/reranking | Reject in hot path | Network-free, deterministic operation remains mandatory |

## 4. Axon baseline and factual gap map

Axon currently contains approximately 14,921 C++ source/header lines, 14 DuckDB tables, 34 MCP
tool declarations and 30 unit/smoke/E2E test files. The implementation is intentionally DuckDB-
specific: 89 direct query call sites and 14 source files directly use DuckDB types or queries. A
provider abstraction across all Axon persistence would therefore be a broad rewrite, not an
additive feature.

### 4.1 Gap map

| Topic | Verdict | Evidence and consequence |
| --- | --- | --- |
| Project-local DuckDB | JÁ EXISTE | `Config.db_path` resolves to `.axon/index.duckdb`; index, graph, memory and dialogue share it |
| Global registry | JÁ EXISTE | `${AXON_REGISTRY_DIR}/registry.json`, repo list, named groups and atomic-ish temp/rename save |
| Stable repository UUID | JÁ EXISTE PARTIALLY | `repository-contract.yaml` provides UUID for Axon, but registry identity is still root/name based |
| `--all` / `--group` HTTP aggregation | JÁ EXISTE | HTTP graph aggregation opens secondary DBs with DuckDB `READ_ONLY` |
| `group_list` / `group_impact` | JÁ EXISTE, DEFECTIVE | `group_impact` reopens secondaries without `READ_ONLY`, queries nonexistent `from_id/to_id`, and swallows all failures |
| Local lock-owner proxy | JÁ EXISTE | authenticated bearer token on ephemeral localhost peer, liveness and timeout handling |
| Idle lock lifecycle | JÁ EXISTE | renewable lease/heartbeat, idle DB release and explicit doctor command |
| Incremental indexing | JÁ EXISTE | `index-paths`, watcher, per-file transactions and delete sweep |
| Project epoch | CONFLITA | current epoch is effectively `max(files.indexed_at)` for capsule cache, not a stable sequence/manifest contract |
| Transactional event/outbox | FALTA | index transactions commit without an event in the same transaction |
| Tombstones | FALTA | deleted files are removed; no durable delete fact exists for a downstream projection |
| Central portfolio projection | FALTA | current aggregation reads live secondaries on demand and has no cursor/history/rebuild state |
| Multiple physical databases | JÁ EXISTE PARTIALLY | registry holds many project DuckDB paths, but there are no role/profile/default contracts |
| Multiple database engines | FALTA, AGORA JUSTIFICADO POR PAPEL | shared PostgreSQL/pgvector, Qdrant and FalkorDB already exist; abstract only derived-store roles, not project persistence |
| Shared central server | JÁ EXISTE PARTIALLY | HTTP/MCP exists, but it serves one project or live aggregate; no owned central projection or remote ingest |
| RRF | JÁ EXISTE | observation memory fuses semantic and lexical ranks with deterministic RRF and evidence |
| Multi-signal capability signatures | FALTA | symbols, routes, edges and embeddings exist separately; no versioned capability aggregate |
| Capability graph import | FALTA | repository contract declares no capability manifest; observed/declarative split absent |
| Capability ownership | UNKNOWN | current repository contract has `capabilities.manifest: null` |
| Schema version table | FALTA | migrations are repeated create/alter attempts; no explicit DB schema ledger |
| Embedding identity | FALTA | model path/dimension are implicit; central comparison could mix incompatible vectors |
| Server auth/TLS/read-only policy | FALTA for public server | current peer token is localhost-specific; Web/HTTP exposure needs a separate hardened contract |
| Immutable tag `v1.2.16` | PRESERVAR | no part of this proposal modifies or moves it |

### 4.2 Mandatory early implementation correction

Before new portfolio query/projection behavior is introduced, G2 must cover `group_impact` with a
regression test and correct it to:

- open every secondary with `duckdb::AccessMode::READ_ONLY`;
- use `edges.from_file` and `edges.to_file`;
- return structured per-repository failures/staleness instead of silently returning an incomplete
  answer;
- validate the target and repository paths;
- reuse the same read-only secondary opener as HTTP aggregation.

This is not scope expansion. It repairs the exact existing component the original proposal says
must not be duplicated.

## 5. Consolidated target architecture

### 5.1 Architectural invariants

1. A project-local DuckDB remains authoritative for observable code state of that project.
2. The Git capability graph remains authoritative for canonical declaration and ownership.
3. Portfolio databases contain derived signatures, evidence references and operational state, not
   a second complete copy of source code.
4. The local portfolio projection and shared-server projection are independently rebuildable.
5. A server never asks clients to mount a shared DuckDB file.
6. Project journals have one author: the project's indexing transaction. No distributed conflict
   resolution is needed for project facts.
7. Notifications are acceleration only. Journals plus reconciliation guarantee convergence.
8. No inferred match creates ownership, moves code, updates the capability graph or approves an
   abstraction.
9. Existing CLI/MCP/HTTP contracts remain compatible and additive.
10. DuckDB remains the universal local default and the only required provider; the governed
    server profile enables PostgreSQL/pgvector, Qdrant and FalkorDB from the first rollout.
11. PostgreSQL is the durable central projection store; Qdrant and FalkorDB are rebuildable read
    models. They are complementary roles, not interchangeable choices.

### 5.2 Separate three concerns

The original proposal should be split into three orthogonal contracts:

| Concern | Question | Proposed contract |
| --- | --- | --- |
| Storage provider | Which engine stores this role? | `StorageProvider` capabilities and conformance |
| Storage target | Which concrete database/endpoint is selected? | named `StorageProfile` with one default per role |
| Federation topology | How do authorities and projections exchange state? | local read-only pull or authenticated remote event/snapshot push |

This prevents `--backend=postgres` from being mistaken for federation, and prevents “central
server” from being implemented as a network-mounted DuckDB.

### 5.3 Storage roles and profiles

Proposed roles:

```text
project_authority    mandatory, per project, local DuckDB
portfolio_local      optional/default derived projection under AXON_REGISTRY_DIR
portfolio_shared     optional derived projection owned by an Axon HTTP/MCP server
semantic_index       optional rebuildable candidate-retrieval index
graph_projection     optional rebuildable traversal/UI read model
capability_declarations read-only imported Git data
```

Illustrative additive registry format:

```json
{
  "schema_version": "axon-registry/v2",
  "repos": [
    {
      "repository_id": "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
      "index_stream_id": "11111111-1111-4111-8111-111111111111",
      "name": "axon",
      "root": "/opt/hideakisolutions/axon",
      "db_path": "/opt/hideakisolutions/axon/.axon/index.duckdb",
      "variant": "main",
      "default_for_profiles": ["local", "team"]
    }
  ],
  "groups": {"core": ["7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb"]},
  "storage_profiles": {
    "local": {
      "role": "portfolio_local",
      "provider": "duckdb",
      "path": "${AXON_REGISTRY_DIR}/portfolio.duckdb",
      "default": true
    },
    "team": {
      "role": "portfolio_shared",
      "transport": "axon_http",
      "endpoint": "https://axon.internal.example",
      "namespace": "hideaki-portfolio",
      "providers": {
        "portfolio_store": "postgresql",
        "semantic_index": "qdrant",
        "graph_projection": "falkordb"
      },
      "default": false
    }
  }
}
```

Compatibility requirements:

- v1 registries load unchanged and are upgraded only on explicit write;
- old fields remain present;
- groups resolve by stable repository id internally, while old name-based membership remains a
  migration input;
- each physical index persists a distinct `index_stream_id`; clones/worktrees may share the logical
  repository id and are registered as explicit variants;
- exactly one index stream is the default for each `(repository_id, storage_profile)` pair; a
  command may select a non-default variant explicitly but it is never merged silently;
- paths and endpoints are never combined in one profile;
- exactly one default may exist for each role, but commands may select a named profile explicitly;
- secrets come from environment/key store, never `registry.json` or CLI arguments;
- a local marker binds a remote profile to server instance id, namespace and protocol version.

### 5.4 Minimal storage provider seam

Do not abstract Axon's entire existing `Database`. Introduce a narrow seam only for derived
portfolio stores and transports:

```text
PortfolioStore
  capabilities()
  open(profile, mode)
  health()
  schema_version()
  transaction(apply_batch)
  stream_cursor(repository_id, index_stream_id)
  replace_repository_stream_partition(repository_id, index_stream_id, snapshot)
  query_capabilities(filter, page)
  query_candidates(filter, page)
  maintenance(kind)
```

Initial providers and transports:

- `duckdb`: project authority and local/offline portfolio projection;
- `postgresql`: shared durable portfolio projection, cursors, journal application, signatures,
  observed/declared matches, candidates and audit state; `pgvector` may provide an exact or
  relationally joined vector path;
- `qdrant`: optional semantic candidate index containing embedding plus bounded metadata and stable
  references, rebuilt from PostgreSQL/signatures and never treated as authority;
- `falkordb`: optional graph read model for implementation, abstraction, package, contract,
  ownership and consumer traversals, rebuilt from the durable projection;
- `axon_http`: authenticated transport to the server, not a storage provider.

This is deliberately a composition by role rather than a generic `--backend` switch. PostgreSQL,
Qdrant and FalkorDB solve different query and concurrency problems. Each optional read model must
have an independently testable disabled/degraded path, and local-only operation must remain fully
functional.

Provider capabilities should be explicit, for example:

```text
transactions, read_only, repository_partition_replace, lexical_search,
vector_search, namespace_isolation, concurrent_writers, remote_service,
maintenance_analyze, maintenance_compact
```

An unsupported requested capability must fail clearly; it must not silently downgrade.

### 5.5 Local event journal

Add schema ledger and event tables in every project DB. Event rows and index mutations must commit
in the same DuckDB transaction.

Minimum tables:

```text
schema_migrations
repository_identity
index_epochs
index_events
index_event_affected
index_manifests
index_tombstones
```

Minimum event envelope:

```json
{
  "schema_version": "axon/index-event/v1",
  "repository_id": "uuid",
  "index_stream_id": "uuid",
  "sequence": 42,
  "event_id": "uuid-or-deterministic-hash",
  "event_type": "IndexFilesUpdated",
  "index_epoch": "epoch-id",
  "previous_epoch": "epoch-id",
  "source_ref": "optional",
  "manifest_hash": "optional",
  "occurred_at": "UTC timestamp",
  "affected": [{"kind": "file", "key": "src/x.cpp", "operation": "upsert"}]
}
```

Use one monotonic sequence per stable physical `index_stream_id`; legitimate main/clone/worktree
streams may share a logical `repository_id`. Enforce uniqueness on both `event_id` and
`(index_stream_id, sequence)`. Store normalized item references rather than unbounded JSON source
content. A snapshot completion event carries the manifest hash. Deletes create tombstones retained
until every configured projection cursor has passed the delete and the retention policy permits
compaction.

### 5.6 Projection databases

Both local and shared projection stores use the same logical schema:

```text
projection_metadata
repositories
repository_cursors
repository_epochs
applied_events
repository_manifests
capability_signatures
capability_evidence
observed_capabilities
declared_capabilities
capability_matches
capability_candidates
capability_scores
capability_drift
projection_tombstones
sync_runs
```

The central transaction:

1. validates logical repository, physical stream identity, protocol/schema and stream sequence continuity;
2. applies one bounded event batch to one repository/stream variant partition;
3. updates signatures, candidates and staleness affected by that batch;
4. inserts applied-event evidence;
5. advances that stream's cursor last;
6. commits atomically.

No acknowledgement is written into a project DB. Cursors belong to each projection. A failed batch
does not advance the cursor. Duplicate batches are no-ops with an explicit replay count.

### 5.7 Sync topologies

#### Local projector

The local projector discovers registered repositories and opens each secondary project DB with
DuckDB `READ_ONLY`. It pulls events after its cursor. A spool/marker or localhost notification can
wake it, while polling and epoch/manifest reconciliation guarantee eventual convergence.

#### Shared server

The shared server cannot assume filesystem access to clients. Clients use authenticated endpoints:

```text
POST /api/v1/portfolio/events          bounded ordered event batch
POST /api/v1/portfolio/snapshot        signature/manifest snapshot for bootstrap or repair
GET  /api/v1/portfolio/cursor/{repo}   default-stream cursor, epoch and resolved index_stream_id
GET  /api/v1/portfolio/cursor/{repo}/{stream} explicit variant cursor and epoch
GET  /api/v1/portfolio/status          authenticated operational status
GET  /healthz                          minimal liveness only
```

The local journal contains durable change references, not a copy of source or every projected row.
For remote publication, a sync assembler reads the committed event range and the corresponding
current metadata from the local DuckDB in read-only mode. It emits a versioned, self-contained
`EventBatch` containing ordered events plus bounded `ProjectionDelta` records (signatures, evidence
references and tombstones). Bootstrap and repair use a versioned `RepositorySnapshot` containing
the complete metadata/signature partition and manifest, followed by the journal tail. Neither
transport contains full code. The server therefore never needs filesystem access to the client.

On ambiguous submission failure, the client queries the server cursor before retrying; it never
falls back to an unrelated direct write. PostgreSQL is the sole durable central writer target in
shared mode and supports concurrent stream ingestion with transactional
`(repository_id, index_stream_id)` cursor updates. The repository-only cursor endpoint resolves the
configured default stream and returns that resolution; variant callers use the explicit endpoint.
The local DuckDB portfolio provider retains its single-writer apply rule; horizontal
multi-writer scale is out of scope only for that embedded provider.

#### Coexistence

A project may publish to both `portfolio_local` and `portfolio_shared`. Each has an independent
cursor. Local failure does not erase shared state; server unavailability does not block local
indexing. Query commands accept `--profile=<name>` and may use `--federated` to merge two projection
result sets. Every result reports source profile, logical repository id, resolved index stream,
repository epoch and freshness. Conflicting variants are not merged invisibly: default-stream
results rank by default, while explicitly federated variants remain separately labeled and any
discrepancy is reported.

### 5.8 Reconciliation and rebuild

Reconciliation is two-level:

- fast: compare repository id, cursor, index epoch and manifest hash;
- deep: request/compute a partition snapshot when the sequence has a gap, epoch lineage breaks,
  manifest differs, schema is incompatible or tombstone retention makes replay impossible.

Required operations:

- selective repository partition rebuild;
- deterministic full projection rebuild;
- dry-run reconcile with reason codes;
- quarantine of invalid event/snapshot input;
- stale marking for unavailable repositories;
- explicit repository removal event, distinct from temporary unavailability;
- compaction only after a retained snapshot and cursor safety check.

### 5.9 Capability signature v1

The signature should be normalized, versioned and split into independently recomputable channels:

```text
identity: repository_id, bounded_context, module, namespace, path, normalized_name
contract: public symbols, signatures, routes, handlers, DTO/schema/event identifiers
structure: AST fingerprints, file/symbol graph neighborhood, call neighborhood
behavior: associated tests, fixtures, observable route/event behavior
dependencies: internal and external dependencies, frameworks and technologies
semantics: deterministic summary plus optional embedding identity/vector reference
evidence: local stable keys, line spans, hashes, provenance and extractor versions
freshness: event sequence, index epoch, manifest hash and extracted_at
```

The projection stores short normalized strings, hashes, vectors and evidence references. It does not
store full file bodies, full test logs, secrets or arbitrary source snippets.

Embedding fields must include model identity, dimension, normalization and distance metric. A
known mismatch prevents vector comparison or routes it to a clearly labeled compatible subgroup.

### 5.10 Candidate generation and classification

Candidate generation should keep independent channels:

- normalized name/token channel;
- semantic embedding channel when compatible;
- public contract channel;
- routes/events/schema channel;
- AST fingerprint channel;
- dependency channel;
- graph-neighborhood channel;
- tests/behavior channel;
- declared ownership/bounded-context prior.

Each channel returns a bounded ranked list. Fuse ranks with deterministic RRF; do not collapse raw
signals into an opaque vector. Apply documented gates after fusion:

- at least two independent positive channels for any duplicate-like class;
- a contract or structural channel is mandatory for `exact_duplicate`;
- bounded-context/domain conflict caps the result at `semantic_coincidence` or
  `insufficient_evidence` unless stronger behavioral evidence refutes it;
- same name alone never promotes a candidate;
- embeddings alone never promote a candidate;
- incompatible embedding identities remove, rather than fake, the semantic channel;
- stable tie-break is candidate id.

Return final RRF score, per-channel ranks/scores, positive and negative evidence, differences,
freshness, classification, confidence, recommendation and invalidation reasons.

### 5.11 Observed versus declared governance

Keep four explicit models:

- `observed_capabilities`: derived only from indexed code;
- `declared_capabilities`: imported read-only from versioned Git fragments;
- `capability_matches`: evidence-bearing mappings;
- `capability_drift`: missing declaration, missing implementation, ownership conflict, stale
  declaration or ambiguous match.

Axon may emit a proposal artifact for capability intake, consolidation, library extraction,
existing-contract adoption, keep-local or ownership review. It cannot write the capability graph,
vault, repository, package registry, ADR status or source code.

### 5.11 Portfolio graph projection and UI

Graphify 0.9.52 was evaluated at commit `680e3ed8edd3dc1fa1961050912941880b778207`.
Its strongest reusable patterns are its repository-qualified global node identities, manifest-based
replace/prune behavior, explicit extracted/inferred confidence, community filtering, search,
neighbor inspection, path/call-flow views and idempotent FalkorDB export. Its generated `graph.html`
is a useful offline artifact, but it is not a durable multi-user portfolio application: it embeds a
bounded snapshot, uses an in-memory NetworkX graph and caps visualization at 5,000 nodes.

Axon should therefore absorb the interaction and provenance model, not embed Graphify as a runtime
dependency. Extend the existing Axon Web surface with server-side, paginated subgraph APIs and a
bundled, offline-capable visualization client. The renderer should be selected by a benchmark
between a WebGL graph library and the existing browser stack; no CDN dependency is permitted.

Required views:

- portfolio topology: repository -> bounded context -> capability -> implementation;
- implementation and abstraction chain: interface/contract -> implementations -> consumers;
- package consumption and version matrix across repositories;
- capability detail with observed, declared, owner, evidence, epoch and provenance;
- duplicate/convergence compare with per-signal scores and important differences;
- consumers, blast radius, shortest paths and upstream/downstream navigation;
- drift, staleness, failed projection and reconciliation status;
- filters for repo/group/domain/language/framework/relation/confidence/freshness;
- proposal queue for human-reviewed intake, consolidation, extraction or keep-local decisions.

The UI reads only Axon APIs. PostgreSQL is the central durable projection, Qdrant supplies bounded
semantic candidates and FalkorDB supplies graph traversals when enabled. Axon joins, validates and
annotates their results with authoritative local/Git provenance. If Qdrant or FalkorDB is down, the
UI remains available with reduced semantic/traversal features and an explicit degraded-state badge.
No UI action writes source, ownership, ADRs or the capability graph without a separate governed
goal and human gate. An optional static HTML export may be added for shareable snapshots.

## 6. Revised delivery graph

The original G0-G11 proposal is sound in direction but should be extended and reordered to make
database roles and server coexistence explicit.

| Goal | Deliverable | Gate/verification emphasis |
| --- | --- | --- |
| G0 | Read-only baseline, Axon-first fallback evidence and gap map | Existing defects recorded without product mutation |
| G1 | Proposed ADRs and authority/storage-role contracts | Owner approval required before Accepted |
| G2 | Registry v2, `group_impact` repair, stable repository ids and named storage profiles | Read-only secondary test, v1 compatibility and duplicate/default validation |
| G3 | Local schema ledger, epoch/manifest and transactional event journal | crash-before/after-commit and zero index-without-event proof |
| G4 | Role-aware store/index/graph ports and conformance harness | capabilities fail closed; no whole-project DB rewrite |
| G5 | DuckDB local portfolio projector and independent cursors | real secondaries always `READ_ONLY` |
| G6 | PostgreSQL central projector and schema migrations | transactional cursor, concurrent ingest and rebuild equivalence |
| G7 | Notification, polling, reconcile and selective/full rebuild | lost marker/event gap and incremental/full equivalence |
| G8 | Capability signature extraction | deterministic normalization/fingerprints and no full-code copy |
| G9 | Qdrant semantic read model plus pgvector baseline | bounded retrieval, identity/dimension checks and deterministic fallback |
| G10 | FalkorDB graph projection and traversal API | idempotent repository replace/prune and no orphan leakage |
| G11 | Multi-signal candidate generation, RRF and classification | bounded channels, deterministic ties and adversarial negatives |
| G12 | Git capability declaration importer, matches and drift | fixtures read-only; zero external writes |
| G13 | Local CLI/MCP/HTTP plus authenticated remote ingest | additive schemas, auth, input/path/threshold validation |
| G14 | Dual local/shared profiles, target markers and fail-soft routing | outage, mismatch and independent convergence E2E |
| G15 | Native portfolio UI and optional static export | server-side bounds, provenance, accessibility and degraded modes |
| G16 | Evals, benchmarks, EN/PT-BR docs and PR closeout | precision/recall, multi-OS CI, security review and rollback |

Every node keeps the original requirements for closed `allowed_paths`, input/output contracts,
objective acceptance criteria, deterministic verify step, rollback, budget, stop condition,
isolated commit and `_graph/<goal-id>/` evidence. Builds and heavy suites remain sequential with at
most `-j2`. No merge, tag, release or deploy is implied.

## 7. CLI and MCP proposal

Preserve the original additive command intent, but make target selection explicit:

```text
axon portfolio profile list|show|validate
axon portfolio sync [--all|--group=<name>] [--profile=<name>]
axon portfolio reconcile [--all|--group=<name>] [--profile=<name>] [--dry-run]
axon portfolio rebuild [--all|--group=<name>] [--repo=<id>] [--profile=<name>]
axon portfolio status [--profile=<name>] [--json]
axon portfolio serve [--profile=<name>] [--host=127.0.0.1] [--port=<n>] [--read-only]

axon capability list [--repo=<id>|--group=<name>] [--profile=<name>]
axon capability search <query> [--profile=<name>|--federated]
axon capability duplicates [--threshold=<0..1>] [--profile=<name>|--federated]
axon capability compare <candidate-id> [--profile=<name>]
axon capability consumers <capability-id> [--profile=<name>]
axon capability drift [--profile=<name>]
```

MCP tools remain equivalent to the original list, with optional `profile` and bounded pagination.
Existing `group_list`, `group_impact`, `--all`, `--group` and HTTP aggregation are reused or routed
through shared components. No existing name or field is removed.

## 8. Test and evaluation additions

In addition to the original mandatory tests, add:

### Provider/profile conformance

- exactly one default per storage role;
- local path and remote endpoint are mutually exclusive;
- unsupported namespace/capability fails;
- remote marker target mismatch fails;
- embedding identity/dimension/metric mismatch is explicit;
- health and maintenance return bounded structured results;
- old registry loads and writes without losing old fields.

### Shared server

- non-loopback without auth refuses to start;
- read-only server hides/refuses mutation;
- Host/Origin and bearer validation;
- bounded body, page and event batch sizes;
- one writer with concurrent clients;
- timeout after commit followed by cursor probe and idempotent retry;
- unavailable server never blocks local project commit;
- local and shared cursors diverge and later converge independently;
- server never exposes project source content or secrets in logs/status.

### Federation correctness

- `group_impact` secondary remains byte-for-byte unchanged after query;
- local projector never opens a secondary writable;
- missing/corrupt/stale repo is represented, not silently skipped;
- repository reidentification has an explicit migration event/gate;
- a deleted repo is not inferred from temporary path unavailability;
- snapshot + tail equals uninterrupted event replay;
- manifest mismatch repairs a syntactically complete but semantically missing event history;
- full rebuild across at least three repos equals incremental projection.

### Capability evals

Keep the proposed positive/negative synthetic corpus and add provider-independent golden signatures.
Report name-only, semantic-only, contract-only and full multi-signal baselines. Measure precision@K,
recall@K, candidate count, extraction latency, incremental sync latency, rebuild time and storage
amplification. Record hardware, compiler, DuckDB version, embedding identity and corpus hash.

## 9. Threat and risk analysis

| Risk | Mitigation |
| --- | --- |
| Network-shared DuckDB corruption/locking | Forbidden topology; only the owning Axon server opens its DB file |
| Central server becomes authority accidentally | Role/schema naming, provenance and docs state projection-only; local code index and Git declarations remain authorities |
| Central outage blocks development | Local indexing/event commit never waits for network; sync is retryable and fail-soft |
| Wrong central target/tenant | Stable server id + namespace marker, TLS/bearer, explicit profile and fail-closed mismatch |
| Event acknowledged but response lost | Query stream cursor before retry; idempotency on index stream id + sequence and event id |
| Writable secondary side effects | Shared `ReadOnlyProjectDatabase` opener plus integration test and filesystem hash evidence |
| Cross-repo path traversal/symlink escape | Canonical roots, registered-root validation, no-follow policy and evidence references confined to repo |
| Central data leakage | Signature allowlist; no full source, secret, PII or arbitrary docstrings/log bodies; structured limited-cardinality telemetry |
| Embedding model drift | Persist model identity, dimension, metric and normalization; isolate incompatible vector spaces |
| Backend abstraction explosion | Role-specific ports only; project DB stays DuckDB; each derived provider has a conformance suite |
| Qdrant/FalkorDB inconsistency | Treat both as rebuildable read models; generation id and source epoch gate every result |
| Cross-store partial publish | Durable PostgreSQL transaction commits first; outbox drives idempotent read-model updates and retry |
| Browser graph overload | Server-side subgraph budgets, pagination, clustering and explicit truncation; never ship the whole portfolio blindly |
| Stale or incomplete projection appears authoritative | Every result exposes profile, cursor, epoch, manifest/freshness and stale reason |
| Name/embedding false positives | Multiple independent channels, negative evidence, bounded-context gate and explicit insufficient-evidence class |
| Tombstone loss after compaction | Snapshot and per-projection cursor safety before journal/tombstone retention advances |
| Registry concurrent writer loss | Move from current last-writer-wins JSON rewrite toward locked/compare-and-swap update in registry v2 |
| Server secrets leak via process list/config | Environment or protected secret file; redact status/logs; never CLI token arguments |
| New public attack surface | Loopback default, auth for metadata, request limits, TLS/private proxy guidance, threat tests |

## 10. Rollout and rollback

Rollout is additive:

1. repair/read-only harden existing aggregation;
2. add schema ledger and journal behind an opt-in feature flag;
3. run shadow local projection and compare it to live aggregation;
4. enable local portfolio queries while preserving old tools;
5. add shared server as opt-in after security gate;
6. enable dual local/shared publication only by explicit profile configuration;
7. enable the governed shared profile with PostgreSQL/pgvector, Qdrant and FalkorDB in shadow mode;
8. compare provider outputs, exercise failure/degraded modes and only then make the shared profile
   the environment default;
9. enable the portfolio UI first read-only, preserving local/offline operation.

Rollback:

- stop projector/server and point commands back to live local aggregation;
- old Axon binaries ignore additive local tables;
- projection DBs can be archived or deleted because they are rebuildable;
- journals remain in project DBs until a later, separately approved compaction policy;
- registry v2 writer preserves v1 fields and can export a v1-compatible view;
- no rollback step moves or mutates tag `v1.2.16`.

## 11. What changes from the original proposal

The supplied proposal is incorporated in full by intent: independent local indexes, transactional
events, derived central projection, notifications plus reconciliation, capability signatures,
multi-signal RRF, observed/declared separation, additive CLI/MCP, governed mini-goals, tests, evals,
observability, security, human gates and definition of done all remain.

The consolidated proposal strengthens it in these specific ways:

1. distinguishes storage engine, concrete target and federation topology;
2. makes local and shared portfolio projections first-class concurrent profiles;
3. defines a safe shared-server topology without network-mounting DuckDB;
4. limits provider abstraction to derived portfolio storage instead of rewriting Axon's 89 direct
   DuckDB query sites;
5. adds explicit provider capabilities, conformance, health, maintenance and mismatch markers;
6. adds embedding identity/distance compatibility;
7. adds remote event/snapshot transport because a server cannot read laptop DB paths;
8. adds ambiguous-submission handling and independent projection cursors;
9. adds registry v2 stable ids and target profiles;
10. identifies and prioritizes the existing `group_impact` read-only/schema defect;
11. separates phase-one single-author journal reconciliation from deferred multi-master/HLC work;
12. adds server security, read-only operation and target isolation as acceptance criteria;
13. uses existing PostgreSQL/pgvector and Qdrant in the governed environment from the first rollout,
    while preserving DuckDB as universal local default;
14. adds FalkorDB as a rebuildable graph read model and a Graphify-informed native portfolio UI;
15. makes provider selection role-aware so relational truth, vector retrieval and graph traversal
    can coexist without ambiguous authority.

## 12. Final recommendation

Adopt MemPalace selectively and at high priority as architectural precedent, not as a runtime or
source dependency. After the read-only G0 baseline is independently accepted, complete the G1
authority/contract proposal and obtain its ADR/intake gates before G2 profile and baseline-defect
implementation.

The recommended production topology has two coexisting profiles:

```text
project DuckDB authorities
        | transactional journals
        +--> local portfolio DuckDB projection (default)
        +--> optional authenticated Axon shared server
                  +--> PostgreSQL/pgvector durable central projection
                  +--> Qdrant semantic read model (optional/rebuildable)
                  +--> FalkorDB graph read model (optional/rebuildable)
                  +--> Axon Web portfolio explorer
```

The local profile is the portable default. In the governed Hideaki environment, the shared profile
is enabled and may be selected as the operational default after shadow verification. This delivers
multiple databases by explicit role, a selectable profile, and a shared centralized server while
keeping both local and central modes alive. It also preserves the most important
truth in the original proposal: the central database is a rebuildable intelligence projection,
never a replacement for project authority or human governance.

## 13. Primary evidence

### MemPalace

- `mempalace/backends/base.py`: provider/collection contract, stable id and namespace isolation,
  typed results, capabilities, health, maintenance and embedding identity.
- `mempalace/backends/registry.py`: selection precedence, registration, instance lifecycle and
  artifact detection.
- `mempalace/backends/{chroma,sqlite_exact,milvus,qdrant,pgvector}.py`: implementations across
  embedded, exact, REST and SQL/server substrates.
- `tests/_backend_conformance.py` and `tests/test_backend_conformance.py`: common behavioral contract.
- `website/guide/remote-server.md`: central authenticated MCP/HTTP topology, read-only mode,
  single writer, TLS/private-network guidance and operational behavior.
- `mempalace/server_registry.py`, `mempalace/mcp_proxy.py`, `mempalace/write_routing.py`: owner
  discovery, thin proxy and direct/prefer/require routing.
- `docs/rfcs/003-agent-logstream-coordination.md`, `mempalace/logstream.py`: bounded append-only
  event/artifact model and cursor semantics.
- `docs/rfcs/004-replicated-palace.md`, `mempalace/logsync.py`, `mempalace/replica.py`: separation of
  canonical facts from derived indexes, stable origin and anti-entropy precedent; full memory
  multi-master remains incomplete upstream.
- `mempalace/searcher.py`, `benchmarks/`: bounded hybrid retrieval and reproducible eval discipline.
- `mempalace/repair.py`, `mempalace/backups.py`: integrity, rebuild, safe file handling and backup
  precedents.
- `deploy/docker-compose.server.yml`, `deploy/mempalace-server.service`: operational server examples.

### Graphify

- `graphify/global_graph.py`: repository-qualified identities, source manifests and incremental
  repository replace/prune behavior.
- `graphify/exporters/html.py`: interactive search, community filtering, neighbors, confidence
  display and the explicit 5,000-node snapshot limit.
- `graphify/exporters/tree_html.py` and `callflow_html.py`: collapsible hierarchy and call-flow
  presentation patterns.
- `graphify/exporters/graphdb.py`: sanitized, idempotent `MERGE` export to FalkorDB/Neo4j.
- `graphify/pg_introspect.py`: database schema as a source of architectural relationships.
- `tests/test_falkordb_integration.py`: real FalkorDB integration and repeat-push behavior.
- `graphify/server/`: bounded multi-project MCP/HTTP serving precedent.

### Governed environment

- `/opt/hideakisolutions/shared-infra/docs/INFRASTRUCTURE_POLICY.md`: applications must consume the
  shared PostgreSQL/pgvector, Qdrant and FalkorDB services rather than declare duplicates.
- `/opt/hideakisolutions/shared-infra/docker-compose.yml`: concrete healthy shared service topology;
  Qdrant is pinned to v1.12.4 and PostgreSQL uses the pgvector PostgreSQL 16 image.
- Runtime verification on 2026-08-30: `shared-postgres`, `shared-qdrant` and `shared-falkordb` were
  running and healthy; the Qdrant collections endpoint responded successfully.

### Axon

- `src/core/db.cpp`: current DuckDB schema and implicit migration approach.
- `src/core/indexer.cpp`: incremental/full transaction boundaries without transactional events.
- `src/core/registry.{hpp,cpp}`: global repo/group registry and live owner metadata.
- `src/mcp/http_server.cpp`: existing `--all`/`--group` aggregation with read-only secondary opens.
- `src/mcp/server.cpp`: existing `group_list`, defective `group_impact`, RRF-backed memory tools and
  additive MCP conventions.
- `src/mcp/peer.cpp`: authenticated localhost owner proxy and bounded peer timeout.
- `src/core/memory_search.cpp`: deterministic semantic/lexical RRF precedent.
- `.specs/decisions/ADR-0001-native-memory-hardening.md` and ADR-0002: authority, compatibility,
  local-first and lock-lifecycle precedents.
- `repository-contract.yaml`: stable repository UUID and currently absent capability manifest.
- `docs/en/architecture.md`, `docs/en/api-reference.md`, `docs/en/faq.md`: published registry,
  aggregation, read-only and server contracts.
