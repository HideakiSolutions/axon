# Native memory hardening baseline

- Source baseline: `main` / `origin/main` at `36ef0994ce5925d6ea6224876ef7bb02ee1e907a`.
- Mode: bounded Loop; one accepted mini-goal at a time.
- Scope: native Axon capture resilience, session idempotency, typed handoffs, hybrid RRF retrieval,
  tests/evals, and documentation.
- Non-scope: sidecar, deployment, release publication, external infrastructure, automatic vault
  writes, HNSW/VSS, watcher replacement, and changes to the immutable `v1.2.16` tag.
- Acceptance: all FR/NFR in `.specs/features/native-memory-hardening/spec.md` evidenced; additive
  contract review; full deterministic tests; independent verification before PR readiness.
- Verification: mini-goal commands in `tasks.md`, followed by clean build, complete CTest, smoke/E2E,
  formatting, security/static checks, and documentation reconciliation.
- Rollback: remove the isolated worktree/feature branch before merge; after merge, revert additive
  code while leaving harmless database columns/tables in place.
- Stop conditions: ambiguous product semantics, non-additive contract requirement, two failed fixes
  for one node, failing unexplained quality gate, or a new production/external-write requirement.
- Authority: implementation and prior commit/PR/merge authorization are present; deployment,
  release/tag publication, and canonical vault mutation remain out of scope.
