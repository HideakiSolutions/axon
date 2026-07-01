# Compression Type Classification Evidence - 2026-06-30

## Scope

Change validated in this note:

- `compress_body` classifies large payloads before lossy compression.
- Binary-like payloads and impossible budgets pass through unchanged.
- Lossy compression is accepted only when the final estimated token count is lower than the original.
- The current diff payload was compared against RTK's closest diff filter.

This note does not claim full shell-output parity with RTK. No native Axon shell wrappers were added in this change.

## Commands

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff -- src/core/compress.cpp tests/unit/test_compress.cpp > /tmp/axon-current.diff
/tmp/axon_compress_probe 500 < /tmp/axon-current.diff > /tmp/axon-current.axon 2> /tmp/axon-current.axon.metrics
rtk diff - < /tmp/axon-current.diff > /tmp/axon-current.rtk
```

The probe uses Axon's `compress_body` and the same `(bytes + 3) / 4` token estimator used by capsule code.

## Results

| Case | Classifier | Input Tokens | Output Tokens | Latency | Result |
|------|------------|--------------|---------------|---------|--------|
| Current real diff via Axon `compress_body(..., budget=500)` | `diff` | 3,997 | 500 | 1 ms | Saved tokens, stayed within requested budget |
| Current real diff via `rtk diff -` | n/a | 3,997 | 3,539 | not measured | Saved fewer tokens for this payload |

Full CTest result:

```text
100% tests passed, 0 tests failed out of 5
```

## Fidelity Checks

- Unit tests assert source-code compression keeps declarations, return lines, and elision markers.
- Unit tests assert binary-like input is returned byte-identical.
- Unit tests assert `token_budget <= 0` returns the original input.
- Unit tests assert changed lossy output has fewer estimated tokens than the source.

## Remaining Gaps

- CCR artifact/retrieve was added later on 2026-06-30; see `docs/evidence/ccr-artifact-retrieve-2026-06-30.md`.
- Per-layer telemetry was added later on 2026-06-30; see `docs/evidence/telemetry-layers-2026-06-30.md`.
- Native Axon shell filtering started later on 2026-06-30 with `axon filter`; see `docs/evidence/shell-filter-diff-2026-06-30.md`. More command families still need RTK benchmarks.
