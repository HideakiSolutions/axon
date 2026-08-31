# G4 Checkpoint — Provider Ports and Reference Conformance

- Mini-goal: G4.
- Facts observed: G2 already selects providers by role and G3 defines repository stream, cursor,
  event, epoch and manifest facts, but no executable provider behavior contract existed.
- Changes performed: added infrastructure-free domain types, a portfolio-store application port and
  a deterministic in-memory reference adapter. The port advertises its role, supported operations,
  health, schema and protocol versions. Apply uses contiguous optimistic cursors and global event-id
  receipts; exact replay is idempotent; replace is repository-stream scoped; unsupported maintenance
  fails explicitly. All mutations are validated against bounded inputs before state publication.
  Correction 1 replaces reference-only inspection with a portable bounded stream inspection and
  makes the exact conformance source factory-driven for reuse by every future adapter. Correction 2
  rejects zero/exhausted cursor transitions and makes versions, maintenance advertisement,
  truncation and field bounds executable contract assertions.
- Tests executed: 8/8 factory-driven conformance tests including actual idempotency collisions,
  partial/mixed replay, atomic invalid tail, independent-stream replace and real bounds;
  configure-time forbidden-infrastructure
  include check, Release full build `-j2`, complete CTest 33/33 with no failures and 2 optional
  embedding skips, `git diff --check` and immutable-tag check all pass.
- Executor result: passed after correction 2/2. The final independent verifier reproduced the prior
  failures, added a max-minus-one multi-event overflow probe, and accepted the node without an
  objective blocker.
- Risks/gaps: this node deliberately supplies no persistent or network adapter. DuckDB local,
  PostgreSQL shared, Qdrant semantic and FalkorDB graph implementations remain G5/G6/G9/G10.
- Functional percentage: approximately 15% of the new runtime scope.
- Rollback: revert the isolated G4 commit; no schema, service or external state is touched.
- Commit: isolated `feat(portfolio): define projection provider contracts`; the legacy untracked
  state event was explicitly excluded.
- Next action: begin G5 DuckDB local portfolio projector in a separate node.
- Human authority: no additional authority is required for this pure additive node. Infrastructure,
  secrets, external writes, merge, deploy, tag and release remain prohibited.

## Owner-authorized manifest correction

- G5 discovery proved that non-snapshot `index-event-v1` and `projection-delta-v1` facts carry epoch
  but not a mandatory manifest. The owner explicitly reopened G4 beyond budget 2/2 for this mismatch.
- `ProjectionEvent.manifest` is now optional. A delta without one advances cursor/epoch and preserves
  the last verified snapshot manifest; an initial delta keeps manifest empty rather than fabricating
  one. `RepositorySnapshot` still requires a bounded manifest.
- 9/9 focused tests and complete CTest 33/33 pass. The independent verifier additionally exercised
  200 manifest-less deltas, later replacement, conflicting replay and bounds, then accepted the
  correction without an objective blocker.
- Commit: isolated corrective `fix(portfolio): preserve snapshot manifest across deltas`; G5 and
  legacy untracked evidence were explicitly excluded.
