# Portfolio Capability Intelligence State

Status: G3 independently accepted and committed; G4 ready

## Current Mini-Goal

G4 — provider ports and reference conformance.

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
- G4-G16 implementation: not started.

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

## Risks and Gaps

- Governance sources reference Constitution v2.2/ADR-0022 and a pinned corpus validator absent from
  the checked-out enterprise-hseos source; current Git sources provide Constitution v2.1 and the
  platform migration view/intake v2 contract.
- Repository reidentification/removal remain explicit later workflows; G3 does not infer identity
  changes or removal from temporary unavailability.
- Provider namespaces and credentials are intentionally unprovisioned.
- PostgreSQL/Qdrant/FalkorDB production adapter dependencies/build strategy require accepted ADR and
  supply-chain review before code.

## Rollback

Remove the unmerged feature worktree/branch or revert the documentation-only G0/G1 commits. No
runtime/schema/infrastructure mutation has occurred.

## Next Action

Start G4 provider ports and reference conformance without mixing later provider implementations.
