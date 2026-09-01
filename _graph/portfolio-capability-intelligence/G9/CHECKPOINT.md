# G9 Checkpoint — Optional Semantic Read Models

- **Mini-goal:** optional semantic providers and fail-soft routing; accepted.
- **Facts observed:** Shared Infrastructure provides PostgreSQL/pgvector and Qdrant. Its policy
  prohibits declaring local duplicate providers. Axon already has an optional PostgreSQL portfolio
  projection adapter, but no Axon-owned semantic namespace or configured Qdrant client.
- **Changes performed:** implemented provider-neutral cosine identity validation, pgvector storage
  with fail-closed dimension checks and atomic migration repair, Qdrant storage with idempotent
  length-framed point identity, and a fail-soft router. PostgreSQL is kept current as the durable
  fallback; a remote mutation failure marks the accelerator dirty, so reads remain on PostgreSQL
  until an explicit successful reconcile marks it current.
- **Tests executed:** real shared-provider integration `test_portfolio_semantic` passed 5/5;
  provider-disabled no-CURL build passed (3 local tests, PostgreSQL integration correctly skipped
  without its explicit DSN); `git diff --check` passed.
- **Independent verifier:** accepted after reproducing and then rechecking remote outage/recovery,
  stale-delete prevention, pgvector migration locking, Qdrant identity and the no-CURL build.
- **Risks/gaps:** the router exposes an explicit reconciliation marker; the projector/reconcile
  workflow must call it only after replay has repaired the accelerator.
- **Functional percentage:** G9 implementation and its acceptance checks complete.
- **Rollback:** remove the unmerged feature commit; external resources are isolated Axon namespaces.
- **Next action:** commit G9, then use this optional semantic layer in candidate generation.
- **Human authority needed:** none for the local commit.
