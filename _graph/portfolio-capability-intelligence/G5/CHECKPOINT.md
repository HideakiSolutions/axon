# G5 Checkpoint — DuckDB Local Portfolio Projector

- **Mini-goal:** G5 accepted and committed after one owner-authorized correction beyond budget 2/2.
- **Facts observed:** project journals can be consumed by physical `index_stream_id` while logical
  repository identity changes. DuckDB objects for one path must be shared in-process. Source
  metadata, journal identity chain and every typed handoff invariant must be validated over one
  read-only transaction before any central write.
- **Changes performed:** implemented the migration-ledger-backed derived DuckDB store; globally
  unique fixed-size BLAKE3 receipts; bounded atomic apply/replace/reidentify; shared database handles
  by canonical path; strict root/symlink/schema/JSON validation; incremental sync, stale recovery,
  removal/return, reconcile and deterministic rebuild. Ordinary batches commit atomically;
  reidentification is an explicit transaction boundary. Projector preflight and store now use one
  shared typed validator. No project source or authority index is written or copied centrally.
- **Tests executed:** six correction-focused tests passed; the complete G5 target passed 27/27;
  quick CTest passed both portfolio store tests; `git diff --check` passed. The prior full regression
  registered 34 tests with 32 passed, 2 optional memory tests skipped and 0 failed. Builds/tests used
  `ccache`, `nice -n 10` and `-j1`.
- **Independent verifier:** ACCEPT. The former invalid-handoff probe now fails before any write with
  `old_cursor=0` and `new_cursor=0`. The verifier confirmed the shared validator and matrix over
  identity, UUIDs, sequence, reason enum, bindings, approval, event ID, epoch and manifest and made
  no worktree changes.
- **Risks/gaps:** no G5 MAJOR remains. PostgreSQL durability/concurrency, remote notification and
  distributed provider behavior belong to G6/G7. `clang-format` is unavailable in the environment;
  formatting installation was not authorized.
- **Functional percentage:** 100% of the bounded G5 output/criteria accepted; overall goal remains
  incomplete because G6-G16 have not been delivered.
- **Rollback:** delete the derived portfolio DuckDB and revert the isolated G5 commit. Project
  journals remain authoritative, read-only to the projector and byte-identical.
- **Next action:** validate G6 PostgreSQL hermetic test isolation and applicable
  infrastructure/security gates before implementation.
- **Human authority needed:** none for the local isolated G5 commit. G6 must stop before shared
  compatibility smoke, credentials, service mutation or schema/data migration without the
  corresponding explicit authority.
