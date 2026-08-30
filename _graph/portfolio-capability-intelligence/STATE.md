# Portfolio Capability Intelligence State

Status: G1 independently accepted; ADR and capability-intake human gates pending

## Current Mini-Goal

G1 — specifications, ADR and capability intake.

## Progress

- G0 baseline and confrontation: independently accepted and committed as `37a5039`.
- G1 specifications/contracts: exceptional third correction authorized, independently accepted and
  ready for its isolated commit.
- G2-G16 implementation: not started; prohibited until ADR/intake gate resolves.

## Current Evidence

- Consolidated MemPalace/Graphify/environment study is present.
- Release build passed at the immutable source baseline with one recorded preexisting warning.
- CTest baseline registered 31 tests: 29 passed and 2 embedding-dependent tests were disabled.
- Specs contain 54 FRs, 16 NFRs, 16 mandatory acceptance scenarios, architecture design,
  five versioned data/transport schemas and G0-G16 delivery contracts.
- Logical repository identity is distinct from physical `index_stream_id`; main/clones/worktrees may
  coexist as explicit variants with independent cursors and one profile-selected default.
- ADR-0003 is `Proposed`.
- Capability Intake v2 proposal requests promotion to platform-core, records that all four exact
  migration-view queries returned no matches and the validator failed on two unrelated preexisting
  registry drifts, only Axon as an evidenced consumer, unavailable mandatory semantic corpus and
  the Proposed ADR. Governance discovery is therefore degraded/fail-closed, not successful.

## Risks and Gaps

- Governance sources reference Constitution v2.2/ADR-0022 and a pinned corpus validator absent from
  the checked-out enterprise-hseos source; current Git sources provide Constitution v2.1 and the
  platform migration view/intake v2 contract.
- Existing `group_impact` correctness/read-only defect is scheduled for G2.
- Provider namespaces and credentials are intentionally unprovisioned.
- PostgreSQL/Qdrant/FalkorDB production adapter dependencies/build strategy require accepted ADR and
  supply-chain review before code.

## Rollback

Remove the unmerged feature worktree/branch or revert the documentation-only G0/G1 commits. No
runtime/schema/infrastructure mutation has occurred.

## Next Action

Create the isolated accepted G1 commit, then obtain explicit owner acceptance/revision of ADR-0003
and platform-owner disposition of the capability intake. G2 remains prohibited until both gates
resolve.
