# G1 Checkpoint — Specifications, ADR and Capability Intake

- Mini-goal: G1.
- Facts observed: exact migration-view queries for capability, contract, module and owner returned
  no matches. Every invocation exited nonzero because of two unrelated preexisting registry drifts;
  governance discovery is degraded/fail-closed. The mandatory pinned semantic corpus validator is
  also absent.
- Changes performed: Proposed ADR, spec/design/tasks, intake v2, five schemas and full FR/NFR/AC
  traceability. Logical repository identity is separated from per-index publication streams so
  main/clones/worktrees retain independent cursors. No product code, external graph, provider,
  database or infrastructure changed.
- Tests executed: Draft 2020-12 meta-validation; positive cross-schema transport fixtures;
  negative snapshot/reidentification constraints; capability-intake v2 validation; exact graph
  query reproduction with expected empty matches/nonzero drift result; 86-identifier trace audit;
  `git diff --check`.
- Executor result: all listed G1 checks pass.
- Independent verifier: `ACCEPT` after the explicitly authorized additional correction. The study's
  registry/store/cursor API/PostgreSQL contracts now use `repository_id + index_stream_id`; no
  material G1 blocker remains.
- Risks/gaps: ADR/intake remain Proposed; mandatory semantic discovery unavailable; only one
  evidenced consumer; identity/authorization/secrets review is required for principals, bindings,
  grants and HMAC lifecycle; provider dependency/supply-chain decision remains inside the Proposed
  ADR; shared namespaces/credentials and live smoke remain gated.
- Functional percentage: 0% of new runtime behavior; G1 establishes contracts only.
- Rollback: revert the future isolated G1 documentation commit; no runtime rollback is required.
- Next action: create the isolated G1 commit, then request explicit ADR and capability-intake
  decisions. G2 remains prohibited until those human gates resolve.
- Human authority: the exceptional G1 correction is explicitly authorized. After G1 acceptance,
  explicit ADR acceptance and platform-owner capability-intake disposition are
  required before G2; migration, secrets, infrastructure, deploy, merge, tag and release remain
  separately gated.
