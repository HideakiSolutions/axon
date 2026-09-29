# Shell Filter Aggregate Benchmark Evidence - 2026-07-01

## Scope

- Add a single replay runner for all native shell-filter command families.
- Validate Axon output with machine-readable `--metrics=json`.
- Verify each lossy Axon output stays within budget and saves tokens.
- Verify CCR recovery returns the original raw trace byte-for-byte whenever Axon reports `recoverable=true`.
- Compare against RTK for each command family in the same run when the RTK wrapper is available.

Runner:

```bash
bash scripts/benchmark_shell_filters.sh /tmp/axon-shell-filter-bench-run
```

## Results

Run timestamp: 2026-07-01T01:15:34Z.

| Case | Budget | Input Tokens | Axon Tokens | Saved | CCR | RTK Status | RTK Tokens |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| diff | 500 | 21,536 | 500 | 21,036 | true | ok | 14,209 |
| grep | 600 | 19,146 | 600 | 18,546 | true | ok | 4,412 |
| json | 600 | 26,029 | 406 | 25,623 | true | ok | 1,468 |
| tsc | 500 | 657 | 369 | 288 | true | ok | 88 |
| test | 700 | 13,346 | 590 | 12,756 | true | ok | 32 |
| package | 300 | 1,113 | 205 | 908 | true | ok | 1,112 |
| lint | 180 | 2,361 | 151 | 2,210 | true | ok | 81 |
| log | 700 | 4,895 | 400 | 4,495 | true | ok | 26 |

Totals:

- Input tokens: 89,083
- Axon output tokens: 3,221
- Axon tokens saved: 85,862
- RTK output tokens for comparable cases: 21,428

## Fidelity Checks

- The runner fails if any Axon case does not change, does not save tokens, or exceeds its budget.
- The runner fails if CCR artifact retrieval differs from the original raw trace.
- The runner writes per-case raw input, Axon output, Axon JSON metrics, RTK output, and RTK stderr under the output directory.
- JSONL summary is emitted at `<out_dir>/shell-filter-benchmark.jsonl`.

## Notes

- Some raw traces come from the current workspace (`git diff`, `rg`, package lockfile, npm dry-run, journalctl).
- Where external traces or tools are unavailable, the runner creates controlled fixtures so the aggregate gate remains runnable locally.
- Per-family evidence files remain the more detailed fidelity record for each parser.
