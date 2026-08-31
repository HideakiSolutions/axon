# G6 Checkpoint — PostgreSQL Shared Projector

- **Mini-goal:** G6 accepted and committed after one correction.
- **Facts observed:** the governed environment already runs healthy shared PostgreSQL 16/pgvector,
  Qdrant and FalkorDB. Hermetic provider tests require `libpq-dev` and an isolated real server.
  PostgreSQL advisory transaction locks serialize each physical stream across clients. Schema names
  and teardown ownership are security boundaries, not cosmetic configuration.
- **Changes performed:** implemented a `libpq` portfolio store with expand-only v1 core and v2
  outbox/reidentification migrations, independent stream cursors, global event receipts, atomic
  batch/cursor/entity/outbox transactions, replace, reidentification, bounded inspection, health and
  validation. Added a no-volume PostgreSQL 16/pgvector runner limited to 1 CPU/512 MiB with EXIT
  teardown, Linux CI coverage and shared-infra documentation. Reserved namespaces fail before
  connection; destructive teardown requires `axon_test_*` and proof that the same instance created
  the schema under an advisory lock. No shared service or credential was accessed.
- **Tests executed:** the real PostgreSQL binary passed 20/20; its CTest registration passed 1/1.
  Coverage includes the shared 12-test provider conformance, concurrent clients, transaction
  rollback including outbox, persistence/reopen, rebuild-state cycles, invalid namespaces,
  migration checksum failure, additive v1→v2 upgrade and preexisting-schema preservation. `bash -n`,
  ShellCheck and `git diff --check` passed. Every temporary container was removed.
- **Independent verifier:** ACCEPT after correction 1/2. The initial CRITICAL probe proved `public`
  could be created/dropped; the corrected probe reports `PUBLIC_REJECTED` before connection. The
  verifier reproduced the hermetic runner, migration, ownership, outbox and teardown evidence.
- **Risks/gaps:** the separately governed `shared-postgres` compatibility smoke has not run, so
  environment-specific Axon namespace, grants and credential delivery remain unverified. macOS and
  Windows provider compilation remains a G15 multi-OS gate. Outbox delivery/checkpoints belong to
  G7/G9/G10.
- **Functional percentage:** 100% of bounded G6 contracts and hermetic acceptance criteria;
  live shared-environment compatibility is explicitly a separate evidence class.
- **Rollback:** revert the isolated G6 commit and drop only an Axon-owned derived schema. Test
  containers and schemas are already gone. The owner-approved host packages `libpq-dev`, `libpq5`
  and `libssl-dev` remain installed unless separately removed.
- **Next action:** evaluate G7 notification/remote-ingest authentication and security gates.
- **Human authority needed:** none for the G6 commit. A shared compatibility smoke still requires
  explicit authorization for governed credential access and a unique temporary Axon namespace.
