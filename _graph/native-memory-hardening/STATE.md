# Native memory hardening state

Status: PR validated; merge gate pending

## Current mini-goal

Delivery — obtain the explicit merge-gate decision for PR #90.

## Evidence

- Baseline and origin/main agree at `36ef0994ce5925d6ea6224876ef7bb02ee1e907a`.
- Existing implementation has a bounded pending-write queue but deletes the claimed file before
  indexing completes.
- Existing Dialogue Layer has sessions/turns but no idempotency key or typed handoff aggregate.
- Existing `search_memory` is semantic-only with optional all-tag filtering.
- Platform registry has no handoff/retrieval owner; generic idempotency belongs to messaging.outbox
  and has no verified C++ package projection.
- The pinned-corpus validation script referenced by global governance is absent from the current
  enterprise-hseos checkout; this remains a recorded governance tooling gap.
- T1 passed `test_pending_writes`, `test_queue_drain`, and
  `test_queue_drain_registry_scope` (3/3), and the complete `axon` target compiled successfully.
- T1 preserves a claim until acknowledgment, replays interrupted batches, deduplicates paths, and
  quarantines poison batches after five attempts without blocking the next queue.
- T2 core compiled and `test_dialogue` passed with session idempotency and typed handoff lifecycle.
- The complete Axon target compiled after correcting MCP schema brace balance.
- The first MCP smoke retry failed because Python was unresolved in the CMake test scope; explicit
  `Python3::Interpreter` discovery corrected that harness configuration.
- The second MCP smoke retry started correctly but `thread_create` returned an MCP `isError` result.
  Per the two-correction stop condition, no third implementation attempt was made.
- `git diff --check` passes and no test `axon serve` process remains active.
- T2 resumed with diagnostic evidence: `axon init` does not create the index database; the smoke
  fixture was corrected to run the real `axon index` bootstrap.
- T2 passed `test_dialogue` and `test_mcp_handoff` (2/2). The MCP smoke proves additive tool
  schemas, session replay idempotency, handoff create replay, claim/complete/list, and rejection of a
  cross-project working directory.
- T3 passed `test_memory_search` and `test_mcp_memory_search` (2/2) with the real local embedding
  model. Evidence covers lexical normalization, RRF, authority clamping, deterministic tie-breaks,
  additive MCP schemas, DuckDB SQL, tag filtering, exact-term retrieval, and ranking fields.
- T4 added a versioned five-query retrieval corpus and runner. With the real local model,
  semantic-only and hybrid both reached Recall@3 = 1.0 and MRR = 1.0; hybrid median latency was
  approximately 40 ms in the isolated fixture.
- Documentation now agrees with the 33-tool MCP surface and records native capture recovery,
  explainable hybrid retrieval, typed handoffs, additive compatibility, and the explicit human
  boundary for canonical memory promotion in English and Portuguese.
- A legacy-schema regression test proves observations receive authority 1.0, sessions accept
  idempotency keys, and typed handoffs can be created after additive migration.
- Review found and corrected an embedding-retry defect: pending-write claims now remain durable
  when embedding fails and replay embedding work even when file indexing is already idempotently
  complete.
- Final local gates passed: Release build, 29/29 CTest tests with the real model, full shellcheck,
  clang-format 15 over all `src/` and `tests/`, docs freshness, Python byte compilation, and
  `git diff --check`.
- Commit `d4ca241` was pushed on `feature/native-memory-hardening` and PR #90 was opened against
  the unchanged audited baseline `36ef099`.
- All five required remote checks passed: GCC/Linux, Clang/macOS, MSVC/Windows, ShellCheck, and
  clang-format. The PR is clean and mergeable with no review findings or comments.

## Next action

Await the separate human merge-gate decision for PR #90. Do not merge implicitly.
