# G9 Final Evidence — Optional Semantic Read Models

Status: accepted on 2026-09-01.

- PostgreSQL/pgvector remains the durable fallback; Qdrant is optional.
- A primary mutation failure marks the remote provider dirty. Reads use the fallback until the
  projector completes replay/rebuild and invokes `mark_primary_reconciled()`.
- Provider identity is cosine-only, model/dimension/metric/generation-scoped, and Qdrant uses a
  length-framed BLAKE3 UUID over signature, model and generation so an epoch refresh updates a
  point instead of duplicating it.
- PostgreSQL validates `vector(N)` metadata fail-closed and repairs a legacy primary key within a
  transaction protected by a transaction advisory lock.

Verification:

- `test_portfolio_semantic`: 5/5 passed against the isolated Axon PostgreSQL/Qdrant allocations.
- Fresh no-CURL configure/build and local semantic tests passed; optional provider code was absent.
- Independent verifier: ACCEPT. It reproduced failed-upsert and failed-delete recovery paths and
  found no stale-result resurrection, migration race or optional-build regression.

No credentials or payloads are stored in this evidence.
