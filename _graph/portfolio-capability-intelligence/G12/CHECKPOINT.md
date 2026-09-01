# G12 Checkpoint — Git Capability Graph Import and Drift

- **Mini-goal:** read-only declared capability import with immutable Git provenance and typed drift.
- **Facts observed:** Git's ordinary working tree and local replacement refs cannot be treated as
  canonical declaration content. The Git object addressed by the recorded commit and path is the
  authority for an imported declaration.
- **Changes performed:** added a shell-free, cross-platform Git process adapter. The importer
  canonicalizes the registered root, rejects escaping fragments, reads a verified `HEAD`, disables
  replacement refs, validates a regular Git tree entry, and parses only that immutable
  `commit:path` blob. It records source repository, commit and path; observed/declaration matches,
  missing implementations, missing declarations and ambiguity remain distinct derived models.
- **Tests executed:** `test_capability_declarations` passed 5/5 and the matching CTest entry
  passed. The release `axon` target built successfully with `--parallel 1`; `git diff --check`
  passed. `clang-format` was unavailable in the environment, so no formatter claim is made.
- **Independent verifier:** reproduced an initially real replacement-ref counterexample, then
  independently reran it after the fix. It accepted the implementation: the importer returned the
  original two declarations under the reported SHA while a local replacement ref pointed at an
  injected commit.
- **Risks/gaps:** import is deliberately Git-executable dependent and fails closed when Git or the
  immutable object is unavailable. G13 must expose the derived data through bounded, additive
  surfaces; it must not add Git writes or ownership mutation.
- **Functional percentage:** G12 contract and directed acceptance checks complete.
- **Rollback:** revert this isolated commit and rebuild the derived declaration partitions.
- **Next action:** G13 additive CLI, MCP and HTTP capability/portfolio interfaces.
- **Human authority needed:** none for this local, additive commit; graph writes remain prohibited.
