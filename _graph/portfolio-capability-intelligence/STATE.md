# Portfolio Capability Intelligence State

Status: G9 optional semantic read models accepted; ready to commit and proceed to graph/candidate stages

## Current Mini-Goal

G9 — Optional semantic providers, durable fallback and recovery gating; accepted.

## Progress

- G0 baseline and confrontation: independently accepted and committed as `37a5039`.
- G1 specifications/contracts: exceptional third correction authorized, independently accepted and
  ready for its isolated commit.
- G2 implementation: registry v2, shared read-only opener and aggregation repair independently
  accepted after the owner-authorized bounded correction beyond budget 2/2. Structural top-level
  and nested JSON types now fail closed before partial model materialization. The verifier added
  7 adversarial cases and found no issue; the isolated G2 commit was created.
- G3 implementation: additive local journal independently accepted after two bounded corrections
  and committed in its isolated node commit.
- G4 implementation: pure provider contracts/reference adapter independently accepted after two
  bounded corrections and committed in its isolated node commit.
- G6 implementation: real libpq provider, migrations, concurrency, durable outbox and hermetic
  runner independently accepted after one correction.
- G7 entry: the responsible owner selected Keycloak Shared. The implementation will use a
  dedicated Axon realm/client contract, public issuer and internal JWKS validation; no Keycloak
  secret, realm/client mutation or deployment is included in this node.
- G7 implementation: Keycloak RS256 validation, target/binding/stream gating, cursor probe and
  server-side reidentification grants are independently accepted. JOSE/JWT/JWK validation is
  fail-closed for unsupported critical headers, malformed audiences, temporal claims, algorithm,
  key operations and weak RSA key material.
- G5 discovery: the owner authorized the required bounded G4 reopening. The correction is
  independently accepted and committed in its isolated corrective commit. G5 is resumed.
- G5 implementation: real DuckDB store/projector and 27 directed tests are executor-green and
  independently accepted. The owner-authorized third correction shares one typed handoff validator
  between full-journal preflight and store, closing the last partial-prefix counterexample.
- G4 correction 4: the owner-authorized cardinality/reidentification reopening is independently
  accepted. The port now distinguishes 500 events/apply, 10,000 mutations/event and 10,000
  snapshot entities, and defines optional capability-advertised atomic identity transfer keyed by
  the persistent physical stream with typed provenance and idempotent chained replay.
- G8 implementation: deterministic incremental capability signature extraction is independently
  accepted and committed as `7e095a8`.
- G9 implementation: optional pgvector/Qdrant semantic providers are independently accepted.
  pgvector is the durable fallback; Qdrant failures set an explicit dirty state and reads remain
  on pgvector until the projection's successful reconcile/rebuild clears it.

## Current Evidence

- Consolidated MemPalace/Graphify/environment study is present.
- Release build passed at the immutable source baseline with one recorded preexisting warning.
- CTest baseline registered 31 tests: 29 passed and 2 embedding-dependent tests were disabled.
- Specs contain 54 FRs, 16 NFRs, 16 mandatory acceptance scenarios, architecture design,
  five versioned data/transport schemas and G0-G16 delivery contracts.
- Logical repository identity is distinct from physical `index_stream_id`; main/clones/worktrees may
  coexist as explicit variants with independent cursors and one profile-selected default.
- ADR-0003 is `Accepted` by explicit responsible-owner decision on 2026-08-30.
- Capability Intake v2 proposal requests promotion to platform-core, records that all four exact
  migration-view queries returned no matches and the validator failed on two unrelated preexisting
  registry drifts, only Axon as an evidenced consumer, unavailable mandatory semantic corpus and
  the accepted strategic ADR. The responsible owner approved `promote` to `platform-core` while
  external graph writing remains prohibited. Discovery remains degraded/fail-closed, not successful.
- Registry v1 remains round-trippable. Registry v2 models logical repositories, distinct physical
  streams, storage profiles/default variants and target markers with fail-closed validation.
- HTTP and MCP aggregation share a root-contained, symlink-rejecting `READ_ONLY` DuckDB opener;
  tested secondary files remained byte-identical and per-repository failures are structured.
- G2 correction 3 passed 20 registry tests, HTTP/MCP smoke, ShellCheck, `git diff --check` and full
  CTest: 29 passed and 2 embedding-dependent tests skipped; no failures.
- G3 persists logical/physical identities, deterministic manifests/epochs, transaction-bound events
  and tombstones across full, incremental, delete, route, embedding and call-resolution mutations.
  Present repository contracts are revalidated on every open and malformed/divergent identities
  fail closed. Thirteen real-DuckDB tests, four call-resolution tests and the full 32-test CTest
  registry pass with no failures (2 optional embedding skips); the final verifier accepted the node.
- The uncommitted G5 target passes 27/27 tests under `ccache`, `nice -n 10`, `-j1`, including three
  byte-identical read-only source databases, replay/rollback, reconcile/rebuild, stale lifecycle,
  removal/return, root containment, symlink rejection, metadata/journal identity convergence,
  ordinary batch atomicity, shared live database handles and canonical handoff mutations. Full
  sequential CTest registered 34 tests: 32 passed, 2 project-declared optional memory tests
  skipped and 0 failed.

## Risks and Gaps

- Governance sources reference Constitution v2.2/ADR-0022 and a pinned corpus validator absent from
  the checked-out enterprise-hseos source; current Git sources provide Constitution v2.1 and the
  platform migration view/intake v2 contract.
- Repository reidentification/removal remain explicit later workflows; G3 does not infer identity
  changes or removal from temporary unavailability.
- Semantic provider namespaces are provisioned as isolated Axon resources. Credentials remain in
  `pass`, are never versioned, and are used only as ephemeral test environments.
- The future projector must invoke the semantic router reconciliation marker only after its replay
  has restored the optional accelerator.
- G7 semantic discovery is degraded because the local second-brain Qdrant/Ollama search endpoint
  was unavailable after a successful status probe; direct canonical Keycloak/shared-infra sources
  were used and this is not treated as a successful semantic result.
- G5 source prevalidation and the DuckDB store now share every typed reidentification invariant.
  The prior independent probe fails before any write with old and new cursors both zero. No G5
  MAJOR remains; reidentification is an explicit documented transaction boundary.
- G6 installed owner-approved `libpq-dev` and used only no-volume resource-bounded PostgreSQL
  16/pgvector test containers. Twenty real-provider tests pass; all containers were removed. The
  shared PostgreSQL service and credentials were not accessed. Reserved schemas and unsafe teardown
  fail closed; v1→v2 upgrade, multi-client concurrency and durable outbox are verified.

## Rollback

Remove the unmerged feature worktree/branch or revert its isolated commits. Dedicated shared
provider allocations may be retained for future G10+ integration or explicitly deprovisioned by
the shared-infrastructure owner.

## Next Action

Commit G9, then begin graph construction and multi-signal candidate generation. Keep deployment,
release and external capability-graph writes behind their separate gates.
