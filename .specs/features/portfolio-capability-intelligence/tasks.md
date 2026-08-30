# Portfolio Capability Intelligence — Governed Delivery Graph

## Global Contract

- Baseline: `f776d73a95628e14328ed4b9f66d89318c437832`.
- Feature branch: `feature/portfolio-capability-intelligence`.
- Execution: one mini-goal at a time; heavy build/test stages sequential, maximum `-j2`.
- Evidence: `_graph/portfolio-capability-intelligence/<goal>/` only; each
  `<goal>/events.jsonl` is append-only.
- Commit rule: one accepted mini-goal per conventional commit after repository gates.
- Verification rule: every mini-goal must record the requested checkpoint fields, run its
  deterministic `verify_step`, receive read-only independent refutation, correct at most twice and
  append the verifier verdict before it may be marked accepted or committed. Executor-only success
  is never sufficient.
- Checkpoint fields required for every node: current mini-goal; observed facts; exact changes;
  executed tests; independent-verifier result; discovered risks/gaps; functional percentage;
  available rollback; next action; and exact human authority needed. Missing fields fail the node.
- Universal rollback: revert the isolated node commit or remove the unmerged worktree/branch.
- Universal stop: ambiguous authority, incompatible public/schema change, two failed correction
  attempts, unexplained regression, secret/infrastructure/migration/deploy need, or human gate.
- No merge, push, release, tag, deployment, external graph write or secret access is authorized by
  this graph.

## Dependency Graph

```text
G0 -> G1 -> G2 -> G3 -> G4 -> G5 -> G6 -> G7
                         |      |      |
                         +------+-G8--+--> G9 -> G10 -> G11 -> G12 -> G13
                                                     |              |
                                                     +---- G14 -----+
                                                                  -> G15 -> G16
```

## Requirement Allocation

The detailed requirement-to-test mapping is authoritative in `traceability.md`; the delivery nodes
own these requirement sets:

| Node | Requirements and acceptance scenarios |
| --- | --- |
| G0 | baseline evidence for NFR-008, NFR-009, NFR-014, AC-016 |
| G1 | FR-001–FR-054 contracts; NFR-001–NFR-016; AC-001–AC-016 specification |
| G2 | FR-014, FR-022, FR-023, FR-029, FR-030, FR-045, FR-054; NFR-006, NFR-008; AC-005, AC-006, AC-013, AC-016 |
| G3 | FR-002–FR-010; NFR-001–NFR-003, NFR-008; AC-001, AC-006, AC-007 |
| G4 | FR-022, FR-028; NFR-002, NFR-012; provider conformance support |
| G5 | FR-011, FR-013–FR-020, FR-023; NFR-002–NFR-005; AC-002–AC-007 |
| G6 | FR-012, FR-015, FR-016, FR-018–FR-020, FR-024, FR-027, FR-030; AC-002, AC-004, AC-007 |
| G7 | FR-017–FR-021, FR-029; NFR-002, NFR-005, NFR-006; AC-003, AC-004, AC-008, AC-013 |
| G8 | FR-031–FR-033; NFR-003, NFR-007, NFR-010; signature portions of AC-009–AC-011 |
| G9 | FR-025, FR-027, FR-034, FR-040; NFR-004, NFR-005, NFR-010, NFR-012; AC-012 |
| G10 | FR-026, FR-034, FR-048–FR-050; NFR-005, NFR-010, NFR-012; AC-012, AC-014 |
| G11 | FR-034–FR-040; NFR-003, NFR-010, NFR-013; AC-009, AC-011, AC-015 |
| G12 | FR-041–FR-044; NFR-006–NFR-008; AC-010 |
| G13 | FR-021, FR-045–FR-047, FR-053; NFR-006, NFR-008, NFR-016; AC-008, AC-011, AC-013, AC-016 |
| G14 | FR-048–FR-052; NFR-005, NFR-015, NFR-016; AC-012, AC-014 |
| G15 | all FR/NFR regression evidence and AC-001–AC-016 |
| G16 | DoD documentation, rollback, residual risks and exact remaining gates |

## G0 — Baseline and Confrontation

- **allowed_paths:** `docs/evidence/**`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** user goal; MemPalace/Graphify checkouts; Axon main at baseline; governance,
  registry and corpus discovery results.
- **output_contract:** consolidated study, immutable baseline, factual gap map, existing defect list,
  provider/environment evidence.
- **criteria:** source baseline recorded; current suite measured; concurrent changes preserved;
  Axon-first fallback explicit; registry/corpus gaps recorded.
- **verify_step:** Release configure/build `-j2`; full CTest; `git diff --check`; tag ref check.
- **rollback:** remove only G0 feature-worktree artifacts.
- **budget:** one discovery/build cycle; no production mutation.
- **stop_condition:** baseline cannot be reproduced or active work overlaps allowed paths.
- **commit:** `docs(portfolio): record federated intelligence baseline`.

## G1 — Specifications, ADR and Capability Intake

- **allowed_paths:** `.specs/features/portfolio-capability-intelligence/**`,
  `.specs/decisions/ADR-0003-federated-portfolio-capability-intelligence.md`,
  `docs/evidence/mempalace-portfolio-capability-intelligence-study-2026-08-30.md`,
  `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** accepted G0 evidence; Enterprise Constitution/standards; Platform Capability
  Intake v2; CQRS/read-model precedents.
- **output_contract:** spec, design, tasks, event/signature schemas, Proposed ADR, proposed intake,
  rollout/rollback and threat model.
- **criteria:** every original requirement maps to FR/NFR/task; schemas validate; ADR remains
  Proposed; no external graph write.
- **verify_step:** JSON schema syntax/meta-validation, intake validation, requirement trace audit and
  independent verifier refutation.
- **rollback:** revert documentation-only node.
- **budget:** two documentation correction attempts.
- **stop_condition:** ADR/intake requires unresolved owner decision.
- **commit:** `docs(portfolio): specify federated capability intelligence`.
- **human gate:** explicit ADR acceptance and capability-intake disposition before G2.

## G2 — Existing Aggregation Repair and Registry v2

- **allowed_paths:** `src/core/registry.*`, `src/mcp/server.cpp`, `src/mcp/http_server.*`,
  `tests/unit/test_registry.cpp`, `tests/smoke/test_http_status_codes.sh`, `tests/CMakeLists.txt`,
  `CMakeLists.txt`, `.specs/features/portfolio-capability-intelligence/**`,
  `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** accepted ADR/intake; current registry/group contracts.
- **output_contract:** shared read-only secondary opener; fixed `group_impact`; additive registry v2
  logical repository ids, physical index stream ids, default variant selection and named profile
  validation.
- **criteria:** secondaries byte-identical after queries; failures structured; v1 round-trip intact;
  one default per role and one stream default per logical repository/profile; legitimate
  main/worktree streams coexist; duplicated stream binding and target mismatch fail.
- **verify_step:** targeted registry/group HTTP/MCP tests plus filesystem hash assertion.
- **rollback:** revert G2 commit; v1 files remain valid.
- **budget:** two correction attempts; no database schema.
- **stop_condition:** public contract would require removal/rename.
- **commit:** `fix(portfolio): harden read-only aggregation and profiles`.

## G3 — Local Schema Ledger, Epoch, Manifest and Journal

- **allowed_paths:** `src/core/db.*`, `src/core/indexer.*`, `src/core/routes.*`,
  `src/core/embeddings.cpp`,
  `src/core/call_resolver.*`, `src/portfolio/domain/**`, `src/portfolio/application/**`,
  `tests/unit/test_portfolio_journal.cpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`,
  `.specs/features/portfolio-capability-intelligence/**`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** event schema v1; accepted G2 profile/identity.
- **output_contract:** additive ledger, transactional event rows, sequence, epoch/manifest and
  tombstones across full/incremental/delete paths, with sequence/cursor identity scoped to the
  persisted physical index stream.
- **criteria:** no committed index mutation without event; crash before/after commit correct;
  deterministic manifest; old DB upgrade generates one persisted stream id; cloned fresh indexes
  receive distinct streams without changing logical repository identity.
- **verify_step:** real DuckDB unit/integration tests with transaction fault injection.
- **rollback:** old binary ignores additive tables; revert code.
- **budget:** two schema/test corrections; migration remains additive.
- **stop_condition:** destructive migration or event contract break.
- **commit:** `feat(portfolio): add transactional project journal`.

## G4 — Provider Ports and Reference Conformance

- **allowed_paths:** `src/portfolio/domain/**`, `src/portfolio/application/**`,
  `tests/unit/test_portfolio_store_contract.cpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`,
  `.specs/features/portfolio-capability-intelligence/**`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** accepted journal and design provider roles.
- **output_contract:** pure domain types, application ports, in-memory/reference adapter and shared
  conformance suite.
- **criteria:** domain/application have no infrastructure headers; unsupported capability fails;
  cursor/idempotency/replace semantics executable.
- **verify_step:** include-boundary check and reference conformance tests.
- **rollback:** revert isolated new modules.
- **budget:** one port surface plus reference adapter; no speculative provider methods.
- **stop_condition:** interface leaks database/transport types.
- **commit:** `feat(portfolio): define projection provider contracts`.

## G5 — DuckDB Local Portfolio Projector

- **allowed_paths:** `src/portfolio/infrastructure/duckdb/**`, `src/portfolio/application/**`,
  `tests/integration/test_portfolio_duckdb.cpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`,
  `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G3 journal; G4 conformance.
- **output_contract:** local portfolio schema/projector, independent cursors and repository replace.
- **criteria:** real secondaries read-only; duplicate replay safe; removal/return/stale modeled;
  rebuild equals incremental.
- **verify_step:** three temporary repositories with crash/replay/reconcile tests.
- **rollback:** delete derived portfolio DuckDB and revert commit.
- **budget:** measured baseline plus two corrections.
- **stop_condition:** any secondary write or source copy.
- **commit:** `feat(portfolio): project local repository intelligence`.

## G6 — PostgreSQL Shared Projector

- **allowed_paths:** `src/portfolio/infrastructure/postgresql/**`, `src/portfolio/application/**`,
  `tests/integration/test_portfolio_postgresql.cpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`,
  `cmake/portfolio-providers.cmake`, `scripts/dependencies/bootstrap-portfolio-providers.sh`,
  `.github/workflows/build.yml`, `.github/workflows/release.yml`, `docs/dev/SHARED-INFRA.md`,
  `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** provisioned non-secret test target; G4/G5 conformance semantics.
- **output_contract:** migrations, transactional apply/cursor, repository partitions and durable
  read-model outbox.
- **criteria:** concurrent clients idempotent; partial commit impossible; schema upgrade and
  partition rebuild pass; no direct client credentials in public config.
- **verify_step:** hermetic real PostgreSQL provider per test run with deterministic teardown and
  provider conformance; separate shared-service compatibility smoke only after per-run approval.
- **rollback:** test environment teardown; disable shared profile and revert adapter.
- **budget:** sequential hermetic integration run; no project-local runtime service.
- **stop_condition:** test isolation unavailable or shared compatibility smoke lacks authorization.
- **commit:** `feat(portfolio): add shared PostgreSQL projection`.

## G7 — Notification, Remote Ingest and Reconciliation

- **allowed_paths:** `src/portfolio/application/**`, `src/portfolio/infrastructure/http/**`,
  `src/portfolio/delivery/**`, `src/mcp/http_server.*`, `src/main.cpp`,
  `tests/integration/test_portfolio_reconcile.cpp`, `tests/smoke/test_portfolio_ingest.py`,
  `tests/CMakeLists.txt`, `CMakeLists.txt`, `cmake/portfolio-providers.cmake`,
  `scripts/dependencies/bootstrap-portfolio-providers.sh`, `.github/workflows/build.yml`,
  `.github/workflows/release.yml`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G5/G6 stores; security contract.
- **output_contract:** marker/polling, authenticated bounded ingest, epoch/manifest repair, selective
  and full rebuild.
- **criteria:** lost notification repaired; timeout-after-commit resolved by cursor probe; local
  commit never waits for network; stale explicit.
- **verify_step:** failure-injection integration and HTTP smoke.
- **rollback:** disable server profile/ingest and retain journals.
- **budget:** bounded batch sizes and two corrections.
- **stop_condition:** auth/security posture lacks explicit acceptance.
- **commit:** `feat(portfolio): reconcile local and shared projections`.

## G8 — Capability Signature Extraction

- **allowed_paths:** `src/portfolio/domain/**`, `src/portfolio/application/extract/**`,
  `tests/unit/test_capability_signature.cpp`, `tests/fixtures/portfolio/**`, `tests/CMakeLists.txt`,
  `CMakeLists.txt`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** signature schema; current file/symbol/route/edge/test data.
- **output_contract:** deterministic normalizers, fingerprints, evidence and incremental impact.
- **criteria:** repeatable hashes; no full code centrally; affected-only recompute; no-embedding path.
- **verify_step:** golden signature fixtures across at least C++, TS and Python.
- **rollback:** rebuild derived signatures with previous extractor version.
- **budget:** bounded neighborhood and evidence count.
- **stop_condition:** nondeterministic or source-copy requirement.
- **commit:** `feat(portfolio): extract capability signatures`.

## G9 — Qdrant Semantic Read Model and pgvector Baseline

- **allowed_paths:** `src/portfolio/infrastructure/qdrant/**`,
  `src/portfolio/infrastructure/postgresql/**`, `src/portfolio/application/search/**`,
  `tests/integration/test_portfolio_semantic.cpp`, `tests/CMakeLists.txt`,
  `CMakeLists.txt`, `cmake/portfolio-providers.cmake`,
  `scripts/dependencies/bootstrap-portfolio-providers.sh`, `.github/workflows/build.yml`,
  `.github/workflows/release.yml`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** provisioned namespaces; G8 signatures/embedding identity.
- **output_contract:** idempotent semantic upsert/delete, exact pgvector baseline and fallback.
- **criteria:** identity/dimension/metric mismatch explicit; generation/epoch filters mandatory;
  Qdrant outage degrades to pgvector/nonsemantic channels.
- **verify_step:** hermetic real Qdrant/PostgreSQL integration, deterministic teardown and
  provider-disabled tests; separate shared compatibility smoke after authorization.
- **rollback:** test environment teardown; rebuild read model from PostgreSQL.
- **budget:** bounded top-K and payload size.
- **stop_condition:** provider namespace/secret not provisioned.
- **commit:** `feat(portfolio): add semantic capability retrieval`.

## G10 — FalkorDB Graph Projection

- **allowed_paths:** `src/portfolio/infrastructure/falkordb/**`,
  `src/portfolio/application/graph/**`, `tests/integration/test_portfolio_graph.cpp`,
  `tests/CMakeLists.txt`, `CMakeLists.txt`, `cmake/portfolio-providers.cmake`,
  `scripts/dependencies/bootstrap-portfolio-providers.sh`, `.github/workflows/build.yml`,
  `.github/workflows/release.yml`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G8 signatures/dependencies; projection outbox.
- **output_contract:** repository-qualified nodes/edges, idempotent replace/prune, bounded traversal.
- **criteria:** no cross-repo id collision; tombstones remove orphans; generation filters; degraded
  fallback explicit.
- **verify_step:** hermetic real FalkorDB repeat-push/rebuild/traversal integration with teardown;
  separate shared compatibility smoke after authorization.
- **rollback:** test environment teardown; rebuild named graph.
- **budget:** bounded depth/node/edge counts.
- **stop_condition:** raw Cypher exposure or graph namespace unprovisioned.
- **commit:** `feat(portfolio): project portfolio relationships to FalkorDB`.

## G11 — Multi-Signal Candidates and Explanations

- **allowed_paths:** `src/portfolio/domain/**`, `src/portfolio/application/candidates/**`,
  `tests/unit/test_capability_candidates.cpp`, `evals/portfolio/**`, `tests/CMakeLists.txt`,
  `CMakeLists.txt`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G8-G10 signals; truth-set fixtures.
- **output_contract:** bounded channel ranks, weighted RRF, classifications, differences,
  invalidators and confidence.
- **criteria:** deterministic ties; score bounds; same-name/different-domain negative; no single
  signal promotion; no-embedding operation.
- **verify_step:** unit tests and name-only/semantic-only/multi-signal eval comparison.
- **rollback:** disable candidate feature/version and rebuild results.
- **budget:** measured candidate cap per signature/repository.
- **stop_condition:** accuracy target cannot be met after two threshold corrections.
- **commit:** `feat(portfolio): explain capability convergence candidates`.

## G12 — Git Capability Graph Import and Drift

- **allowed_paths:** `src/portfolio/infrastructure/git/**`, `src/portfolio/application/declarations/**`,
  `tests/integration/test_capability_declarations.cpp`, `tests/fixtures/capability-graph/**`,
  `tests/CMakeLists.txt`, `CMakeLists.txt`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** repository-owned fragments and schemas; G11 observations.
- **output_contract:** read-only declared import, evidence-bearing matches and typed drift.
- **criteria:** zero external writes; commit/path provenance; ambiguous matches remain unowned;
  missing declaration/implementation fixtures pass.
- **verify_step:** fixture repo hashes unchanged before/after import and drift assertions.
- **rollback:** clear/rebuild declaration partitions.
- **budget:** registered roots/fragments only.
- **stop_condition:** canonical schema unavailable or write required.
- **commit:** `feat(portfolio): compare observed and declared capabilities`.

## G13 — CLI, MCP and HTTP Surfaces

- **allowed_paths:** `src/main.cpp`, `src/mcp/server.*`, `src/mcp/http_server.*`,
  `src/portfolio/delivery/**`, `tests/smoke/test_portfolio_cli.sh`,
  `tests/smoke/test_portfolio_mcp.py`, `tests/smoke/test_portfolio_http.py`,
  `tests/contracts/portfolio/**`, `tests/CMakeLists.txt`, `docs/api/portfolio-openapi.yaml`,
  `docs/en/api-reference.md`, `CMakeLists.txt`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G7/G11/G12 use cases.
- **output_contract:** additive portfolio/capability commands, tools and versioned endpoints.
- **criteria:** schemas additive; OpenAPI committed; provider/consumer compatibility verified;
  inputs bounded; auth/read-only policies; pagination and typed provider/stale errors.
- **verify_step:** CLI/MCP/HTTP schema snapshots, OpenAPI validation, contract verification and
  end-to-end smokes.
- **rollback:** feature/profile disable preserves all old surfaces.
- **budget:** no removals/renames; response budgets documented.
- **stop_condition:** existing public contract must change incompatibly.
- **commit:** `feat(portfolio): expose capability intelligence surfaces`.

## G14 — Native Portfolio UI

- **allowed_paths:** `src/mcp/http_server.*`, `src/portfolio/delivery/web/**`,
  `tests/smoke/test_portfolio_web.py`, `docs/assets/portfolio/**`, `tests/CMakeLists.txt`,
  `CMakeLists.txt`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G10/G13 bounded APIs; accessibility and offline requirements.
- **output_contract:** topology, detail, compare, consumers/package, drift and status views plus
  optional static export.
- **criteria:** no CDN; keyboard/a11y states; subgraph bounds; provenance and degraded mode visible;
  no governance writes.
- **verify_step:** DOM/accessibility smoke, response-budget tests and visual artifact review.
- **rollback:** serve prior Axon Web and retain APIs.
- **budget:** benchmark renderer against representative corpus before selection.
- **stop_condition:** whole-portfolio browser dump or new frontend framework without ADR.
- **commit:** `feat(web): add portfolio capability explorer`.

## G15 — E2E, Evals, Performance and Security

- **allowed_paths:** `tests/**`, `evals/portfolio/**`, `scripts/benchmark_portfolio.sh`,
  `CMakeLists.txt`, `tests/CMakeLists.txt`, `.clang-tidy`, `CMakePresets.json`,
  `docs/evidence/**`, `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** G3-G14 complete implementation.
- **output_contract:** three-repo E2E, truth-set eval, real-provider integration, failure injection,
  performance/resource/security report.
- **criteria:** all spec test cases plus executable BDD journeys; test pyramid/coverage measured;
  precision/recall baselines; rebuild equivalence; no provider writes to authorities; bounded
  logs/metrics; multi-OS-compatible harness; baseline toolchain deviations are either corrected or
  explicitly accepted in ADR evidence.
- **verify_step:** complete sequential Release suite, eval runner, sanitizers/static/security gates.
- **rollback:** tests/fixtures only; no production data retained.
- **budget:** hardware/corpus measured; no HNSW/VSS objective expansion.
- **stop_condition:** unexplained regression or unmet acceptance after two corrections.
- **commit:** `test(portfolio): verify federated capability intelligence`.

## G16 — Documentation and PR-Ready Closeout

- **allowed_paths:** `README.md`, `CHANGELOG.md`, `SECURITY.md`, `docs/en/**`, `docs/pt-br/**`,
  `.specs/features/portfolio-capability-intelligence/**`,
  `_graph/portfolio-capability-intelligence/**`.
- **input_contract:** accepted G15 evidence and actual final contracts.
- **output_contract:** reconciled architecture/API/getting-started/troubleshooting/security/shared-infra
  docs, completion audit, residual risks and exact human gates.
- **criteria:** full quality gates green; independent verifier cannot refute DoD; worktree clean after
  isolated commits; PR-ready with no merge/release/deploy.
- **verify_step:** Release build, full CTest, format, ShellCheck, static/security analysis, EN/PT-BR
  doc validation, diff/branch/tag checks.
- **rollback:** revert documentation/closeout commit; implementation commits remain isolated.
- **budget:** two closeout corrections.
- **stop_condition:** CI multi-OS or human-gated operation remains unverified.
- **commit:** `docs(portfolio): complete federated intelligence delivery`.

## Dynamic Discovery Rule

Expansion is allowed only for directly relevant existing implementations/call sites, required
schemas/contracts, indispensable missing tests, multiplatform differences or defects caused by this
feature. Deduplicate against all observed items. Stop expansion after two consecutive discovery
rounds add no relevant gap, all acceptance criteria are evidenced, the node budget is reached or a
human/external decision is required.
