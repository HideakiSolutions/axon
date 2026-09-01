# G2 Checkpoint — Aggregation Repair and Registry v2

- Mini-goal: G2.
- Facts observed: `group_impact` opened secondary DuckDB files with default write-capable access,
  queried removed `edges.from_id`/`edges.to_id` columns and silently discarded open/query errors.
  HTTP graph aggregation independently configured `READ_ONLY` but had no shared root/symlink policy
  and exposed no structured secondary failures. Registry v1 had only physical paths/names.
- Changes performed: added an additive registry v2 model for logical `repository_id`, physical
  `index_stream_id`, per-profile default variants, role-aware local/shared profiles and target
  markers; added validation for duplicate stream bindings, missing/ambiguous defaults, provider
  roles and target mismatches; retained v1 serialization when no v2 fields exist. Added one shared
  secondary opener enforcing `READ_ONLY`, canonical registered-root containment and symlink
  rejection. Repaired `group_impact` to use `from_file`/`to_file`; MCP and HTTP now expose typed
  per-repository failures without removing existing fields.
- Tests executed: Release build `-j2`; 20 registry tests including DuckDB real read/write negative,
  byte equality and v1/v2 round trips; HTTP/MCP three-project smoke with one unavailable secondary;
  complete CTest serial run (31 registered, 29 passed, 2 embedding-dependent skipped); ShellCheck;
  `git diff --check`.
- Executor result: all G2 contract checks pass. Clang-format could not be executed because no
  `clang-format` binary/version is installed; touched blocks were manually aligned and diff-check is
  clean.
- Independent verifier: first refutation rejected the executor result because production consumers
  did not enforce validation/default selection, serialized JSON diverged from the accepted
  `axon-registry/v2` contract, groups were still name-only and identity/target validation was
  incomplete. Correction 1 aligns the canonical JSON/design, makes groups UUID-aware with v1 name
  migration, gates production aggregation fail-closed and adds adversarial tests. Re-verification
  passed. The second refutation then reproduced a process abort for syntactically valid JSON with a
  wrong field type and identified incomplete endpoint policy, mixed-membership duplication and the
  stale test count. Correction 2 converts parse/type failures into structured load issues, prevents
  malformed registries from being overwritten, enforces HTTPS except explicit loopback HTTP,
  deduplicates by physical stream and reproduces the exact malformed input in unit and production
  smoke tests. The final independent refutation confirmed that exact case, but rejected G2 because
  a different structural mismatch (`repos` object instead of array) is silently ignored and can be
  overwritten during startup. This violated fail-closed/no-overwrite. The owner explicitly reopened
  G2 for one bounded correction beyond budget 2/2. Correction 3 now validates every structural JSON
  container and nested field consumed by the registry parser before materializing any partial model,
  emits path-specific `invalid_registry_type` issues and preserves malformed input byte-for-byte.
  Executor verification passed the 20-case adversarial matrix, the exact verifier reproduction,
  smoke and complete CTest. The independent verifier then added 7 adversarial cases of its own and
  accepted G2 without findings after confirming structured errors, no partial aggregation, clean
  MCP exit and byte-identical registry/secondary files.
- Risks/gaps: v2 identity is parsed, persisted and validated but generation from
  `repository-contract.yaml` and physical index persistence belong to G3. Profile credentials and
  live shared namespaces remain intentionally unprovisioned. The untrusted remote transport and
  provider adapters remain later mini-goals.
- Functional percentage: approximately 6% of the new runtime scope; G2 provides the safe
  aggregation/profile foundation but no event journal or portfolio projection yet.
- Rollback: revert the isolated G2 commit; v1 registries remain valid and no database schema or
  external system was changed.
- Next action: evaluate the G3 entry contract separately. G2 is isolated in its contracted commit;
  no G3 implementation is included in this node.
- Human authority: ADR-0003 and capability intake promotion to platform-core were explicitly
  approved. No authority was inferred for merge, deploy, release, secrets, shared infrastructure,
  external capability-graph writes or destructive schema change.
