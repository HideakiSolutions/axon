# G3 Checkpoint — Local Schema Ledger, Epoch, Manifest and Journal

- Mini-goal: G3.
- Facts observed: existing full indexing committed file/symbol rows before edges and deleted-file
  pruning; incremental indexing repeated that split; routes and embeddings mutated independently.
  The only prior epoch was `max(files.indexed_at)`, unsuitable for deterministic reconciliation.
- Changes performed: added additive migration, metadata, event and tombstone tables; persisted the
  logical repository UUID from `repository-contract.yaml` and one UUIDv4 stream per fresh physical
  database; added a deterministic BLAKE3 manifest over sorted files, symbols, edges and routes and
  an identity-bound epoch. Full, incremental, delete, route and embedding mutations now share their
  transaction with validated events; file/route deletes create precise tombstones and later upserts
  clear them. SQL mutation failures now propagate to rollback.
- Tests executed: Release full build with `-j2`; 13 real-DuckDB tests covering pre-journal upgrade,
  identity persistence/fresh clones, full/incremental/delete/routes, sequence/event ids, manifest,
  tombstone resurrection, schema enums, contract-envelope/reopen validation and four crash positions;
  4/4 call-resolution tests; complete serial CTest with 32/32 no failures and 2
  embedding-dependent skips; `git diff --check`; immutable tag check.
- Executor result: passed. `clang-format` is unavailable in the environment; compiler checks and
  `git diff --check` are clean.
- Independent verifier: first refutation rejected G3 because direct call-edge resolution could
  mutate without an event, reidentification was not typed, snapshot hash bounds and invalid-contract
  handling were incomplete, and embeddings did not affect manifest. Correction 1 adds a transaction
  mutation/event guard, autojournaled public call resolution, typed atomic reidentification/removal,
  fail-closed identity/hash validation and embedding state in the manifest. The second refutation
  found that a changed contract was not revalidated on reopen. Correction 2 revalidates every
  present contract, rejects logical-id divergence and validates the canonical v1 identity envelope.
  The final independent verifier accepted G3 after reproducing the adversarial cases and auditing
  every production mutation path.
- Risks/gaps: repository reidentification/removal require explicit owner workflows in later delivery
  surfaces; the v1 enum accepts those event kinds but G3 introduces no implicit identity mutation.
  Network notification, central projection and provider adapters remain later nodes. Long full-index
  transactions trade write-lock duration for the required atomicity and require benchmark evidence
  in G15.
- Functional percentage: approximately 12% of the new runtime scope.
- Rollback: revert the isolated G3 commit after acceptance; older binaries ignore the additive
  tables. No shared service, external repository or capability graph was mutated.
- Commit: isolated `feat(portfolio): add transactional project journal`; the legacy untracked state
  event was explicitly excluded.
- Next action: begin G4 provider ports and reference conformance in a separate node.
- Human authority: no additional authority is required for this additive local G3 verification.
  Destructive migration, shared infrastructure, secrets, external writes, merge, deploy, tag and
  release remain prohibited.
