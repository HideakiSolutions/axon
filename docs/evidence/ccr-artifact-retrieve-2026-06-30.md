# CCR Artifact Retrieve Evidence - 2026-06-30

## Scope

Change validated in this note:

- Lossy capsule body compression stores the original slice as a CCR artifact before emitting recoverable compressed output.
- Compressed capsule content includes an `axon:ccr` marker with `artifact_id` and original token count.
- Capsule responses expose `ccr_artifact_ids`.
- Original content can be retrieved through MCP `artifact_retrieve`, CLI `axon artifact-retrieve <artifact_id>`, or REST `GET /api/artifact/<artifact_id>`.
- If adding the CCR marker would remove the token savings, Axon skips lossy compression for that slice instead of emitting unrecoverable output.

This note validates the CCR storage/retrieval path and integration points. It does not claim native shell-filter coverage.

## Commands

```bash
cmake -S . -B build
cmake --build build --target test_ccr -j2
ctest --test-dir build --output-on-failure -R test_ccr
```

## Results

Focused CCR result:

```text
100% tests passed, 0 tests failed out of 1
```

`tests/unit/test_ccr.cpp` verifies:

| Check | Evidence |
|-------|----------|
| Deterministic IDs | Same kind/source/content returns same `ccr_...` ID |
| Content sensitivity | Different content returns a different ID |
| Exact recovery | Stored content round-trips byte-for-byte through `ccr_retrieve_artifact` |
| Idempotent storage | Re-storing the same artifact leaves one row |
| Missing artifacts | Missing IDs return `nullopt` |
| Marker fidelity | Marker includes `artifact_id` and `original_tokens` |

## Fidelity Checks

- CCR artifacts are stored in DuckDB table `ccr_artifacts`.
- Artifact IDs are BLAKE3-derived from kind, source reference, and original content.
- Capsule compression only counts savings after the recoverable output, including marker overhead, remains smaller than the original.
- If artifact storage fails, capsule compression keeps the original slice instead of emitting a dangling recovery marker.
- `artifact_retrieve` returns metadata plus exact original content.

## Remaining Gaps

- Native Axon shell filtering started later on 2026-06-30 with `axon filter`; see `docs/evidence/shell-filter-diff-2026-06-30.md`.
- Full end-to-end capsule CCR smoke with a real indexed project and embedding model should be added once the benchmark harness exists.
