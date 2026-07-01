# Shell Filter Diff Evidence - 2026-06-30

## Scope

Change validated in this note:

- `axon filter <kind> [--budget=N]` reads shell output from stdin.
- The native filter classifies output before compression.
- Unsafe, empty, already-small, or non-beneficial outputs pass through unchanged.
- Filter savings can be recorded in the `shell_filtering` telemetry layer when `AXON_TELEMETRY=1` and a project DB exists.
- The native diff filter was compared against RTK on a real current worktree diff.

This note validates the first native Axon shell filter path. It does not claim parity for every RTK command yet.

## Commands

```bash
cmake -S . -B build
cmake --build build --target axon -j2
git diff -- . ':!third_party' > /tmp/axon-shell-filter-current.diff
./build/axon filter diff --budget=500 < /tmp/axon-shell-filter-current.diff > /tmp/axon-shell-filter.axon 2> /tmp/axon-shell-filter.axon.metrics
rtk diff - < /tmp/axon-shell-filter-current.diff > /tmp/axon-shell-filter.rtk
```

Token estimates use Axon's existing `(bytes + 3) / 4` estimator.

## Results

| Case | Input Tokens | Output Tokens | Result |
|------|--------------|---------------|--------|
| Raw current diff | 16,771 | 16,771 | Baseline |
| `axon filter diff --budget=500` | 16,771 | 500 | Saved 16,271 tokens, stayed within budget |
| `rtk diff -` | 16,771 | 10,732 | Saved 6,039 tokens for this payload |

Axon emitted:

```text
[axon filter] kind=diff input_tokens=16771 output_tokens=500 saved=16271 changed=true
```

## Fidelity Checks

- Unit tests verify diff output is classified as `diff`, reduced, and still contains the `diff --git` header.
- Unit tests verify log output keeps an important `ERROR` line after filtering.
- Unit tests verify small and binary-like inputs pass through unchanged.
- Unit tests verify `rg` normalizes to the `grep` command family for metrics.

## Remaining Gaps

- Grep/rg, JSON, TypeScript compiler, test-output, package-manager, linter, and log filtering were added later; see `docs/evidence/shell-filter-grep-2026-07-01.md`, `docs/evidence/shell-filter-json-2026-07-01.md`, `docs/evidence/shell-filter-tsc-2026-07-01.md`, `docs/evidence/shell-filter-test-2026-07-01.md`, `docs/evidence/shell-filter-package-2026-07-01.md`, `docs/evidence/shell-filter-lint-2026-07-01.md`, and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
