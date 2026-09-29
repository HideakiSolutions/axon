# Portfolio Capability Intelligence — Traceability Matrix

This matrix is the G1 audit contract. “Evidence” names the deterministic artifact expected under
`_graph/portfolio-capability-intelligence/<goal>/`; it does not claim future implementation exists.

## Functional requirements

| Requirement | Contract/artifact | Owner goal | Deterministic verification and evidence |
| --- | --- | --- | --- |
| FR-001 | project authority invariant | G3/G15 | pre/post project DB authority and regression report |
| FR-002 | schema ledger | G3 | old/new real DuckDB migration test |
| FR-003 | transactional journal | G3 | crash-before/after fault-injection evidence |
| FR-004 | per-index-stream sequence | G3 | main/worktree independent ordered sequence test |
| FR-005 | event/idempotency identity | G3/G5 | duplicate event and batch replay test |
| FR-006 | `index-event-v1.schema.json` | G3 | positive/negative schema fixtures |
| FR-007 | event enum | G3 | one mutation fixture per event type |
| FR-008 | tombstone model | G3/G5 | delete/replay/compaction safety test |
| FR-009 | epoch/manifest | G3/G7 | deterministic hash and mismatch repair test |
| FR-010 | ADR outbox boundary | G3/G15 | rebuild never treats journal as write authority |
| FR-011 | local portfolio DuckDB | G5 | real three-repository projector integration |
| FR-012 | shared PostgreSQL | G6 | hermetic PostgreSQL conformance suite |
| FR-013 | independent profile cursors | G5/G6/G14 | dual-target outage/recovery E2E |
| FR-014 | read-only secondaries | G2/G5 | byte hash before/after every secondary query |
| FR-015 | atomic batch/cursor | G5/G6 | injected failure before cursor update |
| FR-016 | replay idempotency | G5/G6 | duplicate event/batch/snapshot replay |
| FR-017 | hint plus polling/reconcile | G7 | discarded marker convergence test |
| FR-018 | selective/full rebuild | G5/G7 | partition/full rebuild runner |
| FR-019 | rebuild equivalence | G5/G7/G15 | normalized state digest comparison |
| FR-020 | stale/degraded state | G5/G7/G14 | repository/provider outage E2E |
| FR-021 | remote self-contained transport | G7/G13 | server-without-client-filesystem ingest test |
| FR-022 | role-aware profiles | G2/G4 | profile parser/provider-capability tests |
| FR-023 | DuckDB default | G2/G5 | v1 migration and default resolution tests |
| FR-024 | PostgreSQL durable store role | G6 | store conformance and concurrent ingest |
| FR-025 | Qdrant disposable semantic role | G9 | rebuild/outage/generation integration |
| FR-026 | FalkorDB disposable graph role | G10 | replace/prune/rebuild integration |
| FR-027 | pgvector exact fallback | G6/G9 | exact-result baseline comparison |
| FR-028 | provider capability/health/maintenance | G4/G5/G6/G9/G10 | common provider conformance suite |
| FR-029 | target marker | G2/G7 | instance/namespace/protocol mismatch negatives |
| FR-030 | consume shared services | G6/G9/G10/G16 | manifest scan plus separately authorized smoke |
| FR-031 | incremental deterministic extraction | G8 | golden fixture repeatability/impact test |
| FR-032 | `capability-signature-v1.schema.json` | G8 | complete positive and missing-channel negative fixtures |
| FR-033 | metadata-only central copy | G8/G15 | payload/log/source-content denylist audit |
| FR-034 | independent signal channels | G9/G10/G11 | channel ablation report |
| FR-035 | bounded deterministic RRF | G11 | score/tie/cap unit tests |
| FR-036 | no single-signal promotion | G11 | name-only and semantic-only negative tests |
| FR-037 | explanation result contract | G11/G13 | response snapshot contains every explanation field |
| FR-038 | six classifications | G11 | fixture for every enum/class boundary |
| FR-039 | same-name/different-domain negative | G11 | adversarial truth-set assertion |
| FR-040 | no-embedding mode | G9/G11 | provider-disabled full candidate run |
| FR-041 | four separate governance models | G12 | schema/table/type separation assertion |
| FR-042 | read-only Git declaration import | G12 | fixture repository hash unchanged |
| FR-043 | bounded recommendation taxonomy | G11/G12 | recommendation enum/schema snapshots |
| FR-044 | forbidden automatic writes | G12/G14/G15 | write-spy/fixture hash/security audit |
| FR-045 | additive CLI commands | G13 | old/new CLI snapshot and E2E |
| FR-046 | additive MCP tools | G13 | old/new tool schema snapshot and E2E |
| FR-047 | authenticated bounded HTTP/TLS policy | G13/G15 | OpenAPI/auth/bounds/TLS configuration tests |
| FR-048 | portfolio graph/UI views | G10/G14 | DOM journey and bounded graph API tests |
| FR-049 | server-side subgraph bounds | G10/G14 | depth/node/edge truncation tests |
| FR-050 | degraded UI | G9/G10/G14 | Qdrant/Falkor outage browser journey |
| FR-051 | read-only UI proposals | G12/G14 | write-spy and authorization negatives |
| FR-052 | optional static export | G14 | deterministic offline artifact test if implemented |
| FR-053 | OpenAPI/message contracts | G7/G13 | schema fixtures and provider/consumer verification |
| FR-054 | logical repository vs physical stream identity | G2/G3 | legacy adoption, main/worktree coexistence, duplicate-stream quarantine and old/new logical-id handoff tests |

## Non-functional requirements

| Requirement | Owner goal(s) | Deterministic verification and evidence |
| --- | --- | --- |
| NFR-001 | G3 | atomicity fault matrix: zero confirmed mutation without event |
| NFR-002 | G3/G5/G6/G7 | duplicate event/notification/snapshot/retry state digest |
| NFR-003 | G3/G8/G11/G15 | repeated manifest/signature/fingerprint/rank/rebuild digests |
| NFR-004 | G5/G9/G11/G14 | network/providers disabled E2E |
| NFR-005 | G7/G9/G10/G14 | outage matrix with explicit degraded/stale status |
| NFR-006 | G2/G7/G12/G13/G15 | root/symlink/input/query/log security negatives |
| NFR-007 | G8/G12/G15 | metadata allowlist and secret/PII/source denylist scan |
| NFR-008 | G2/G3/G13/G15 | old schema/surface snapshots and rollback rehearsal |
| NFR-009 | G6/G7/G9/G10/G15 | Linux/macOS/Windows configure/build/test CI |
| NFR-010 | G8/G9/G10/G11/G15 | affected-partition and fan-out cap benchmarks |
| NFR-011 | G3/G5/G6/G7/G15 | structured event schema and bounded-cardinality audit |
| NFR-012 | G4/G5/G6/G9/G10/G15 | pure domain tests plus hermetic real adapters |
| NFR-013 | G11/G15 | versioned truth set and reproducible precision/recall report |
| NFR-014 | all implementation goals | sequential heavy gates at `-j2`, hardware/corpus recorded |
| NFR-015 | G14/G15 | keyboard, labels, reduced-motion and non-color checks |
| NFR-016 | G13/G14/G15 | executable Given/When/Then journeys; live/contract evidence split |

## Mandatory acceptance scenarios

| Scenario | Owner goal(s) | Executable evidence |
| --- | --- | --- |
| AC-001 | G3 | journal transaction crash matrix |
| AC-002 | G5/G6 | replay state digest/cursor assertion |
| AC-003 | G7 | lost marker plus polling/reconcile test |
| AC-004 | G5/G7/G15 | incremental/selective/full normalized comparison |
| AC-005 | G2/G5 | secondary byte hashes |
| AC-006 | G2/G3/G5 | remove/stale/return/reidentify lifecycle test |
| AC-007 | G3/G5/G6 | previous-version DB fixture upgrades |
| AC-008 | G7/G13 | remote batch/snapshot/tail without filesystem access |
| AC-009 | G11/G15 | three-repository four-class truth set |
| AC-010 | G12/G15 | missing-declaration/missing-implementation fixtures |
| AC-011 | G11/G13 | CLI/MCP/HTTP explanation snapshots |
| AC-012 | G9/G10/G14 | provider-disabled and outage journeys |
| AC-013 | G2/G7/G10/G13/G15 | path/input/query/bounds adversarial suite |
| AC-014 | G10/G14 | keyboard portfolio graph/product journey |
| AC-015 | G11/G15 | name/semantic/multi-signal precision@K/recall@K report |
| AC-016 | G0/G13/G15/G16 | baseline/full regression and multi-OS evidence |

## Original proposal surfaces and governance

| Original obligation | Requirement/task mapping | Evidence |
| --- | --- | --- |
| eight minimum journal events, sequence, epoch, manifests and tombstones | FR-003–FR-010; G3 | event schema fixtures and DuckDB fault tests |
| central repository/cursor/event/signature/evidence/observed/declared/match/candidate/drift/audit state | FR-011–FR-020, FR-031–FR-044; G5/G6/G8/G11/G12 | migration and conformance catalogs |
| notification loss, polling, reconciliation, selective/full rebuild | FR-017–FR-020; G7 | AC-003/AC-004 |
| self-contained remote server mode coexisting with local mode | FR-012, FR-013, FR-021; G6/G7/G14 | AC-008 and dual-profile E2E |
| CLI portfolio profile/sync/reconcile/rebuild/status and capability list/search/duplicates/compare/consumers/drift | FR-045; G13 | CLI command/schema snapshots |
| MCP portfolio_status/sync and capability list/search/duplicates/compare/consumers/drift | FR-046; G13 | MCP tool snapshots |
| additive versioned HTTP and UI for implementations/packages/abstractions | FR-047–FR-053; G13/G14 | OpenAPI, DOM and accessibility tests |
| exact/convergent/shared primitive/specialization/coincidence/insufficient classes | FR-038; G11 | six-class truth set |
| declared vs observed separation and zero automatic governance writes | FR-041–FR-044; G12 | immutable Git fixture hashes |
| unit, real DuckDB/provider integration, E2E, eval and performance corpus | NFR-012–NFR-014, AC-001–AC-016; G15 | versioned reports and runners |
| structured bounded observability without code/secret/PII | NFR-006, NFR-007, NFR-011; G15 | schema/cardinality/redaction audit |
| Linux/macOS/Windows, `-j2`, no silent HNSW/VSS | NFR-009, NFR-014; G15 | CI and benchmark configuration |
| per-node closed paths/contracts/criteria/verify/rollback/budget/stop/commit/evidence | tasks.md G0–G16 | one accepted commit and node evidence each |
| human gates for ADR/intake, migration, security/infra/secrets, cross-repo writes, merge/release/deploy/tag | ADR-0003 plus tasks global/human gates | explicit checkpoint authority field; no inferred approval |
| immutable `v1.2.16` | constraint; G0/G16 | tag object id check |

## Audit rule

G1 verification extracts every `FR-*`, `NFR-*` and `AC-*` identifier from `spec.md` and fails if it
is absent from this file or has no goal and deterministic verification. Later nodes may refine test
paths, but cannot silently drop an identifier.
