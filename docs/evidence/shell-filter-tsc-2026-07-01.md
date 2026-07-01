# Shell Filter TypeScript Evidence - 2026-07-01

## Scope

- `axon filter tsc --budget=N` parses TypeScript compiler diagnostics shaped like `file(line,column): error TSxxxx: message`.
- Diagnostics are grouped by file and diagnostic code.
- Indented TypeScript continuation lines are folded into the owning diagnostic before truncation.
- Malformed/non-diagnostic input passes through safely instead of emitting a lossy compiler summary.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
./build/axon filter tsc --budget=500 < ~/.local/share/rtk/tee/1782438637_tsc.log > /tmp/axon-real-tsc-filtered.txt 2> /tmp/axon-real-tsc-filtered.err
rtk test /tmp/axon-tsc-trace.sh > /tmp/axon-tsc-rtk-test.txt 2> /tmp/axon-tsc-rtk-test.err || true
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

Real TypeScript diagnostic trace from RTK tee history:

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw `~/.local/share/rtk/tee/1782438637_tsc.log` | 657 | 657 | Baseline, 8 diagnostics in 3 files |
| `axon filter tsc --budget=500` | 657 | 340 | Saved 317 tokens, grouped by file and TS code |

Axon stderr:

```text
[axon filter] kind=plain_text input_tokens=657 output_tokens=340 saved=317 changed=true
```

Controlled command trace used for current RTK wrapper comparison:

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw controlled `tsc`-style trace | 3,900 | 3,900 | Baseline, 108 diagnostics in 12 files |
| `axon filter tsc --budget=500` | 3,900 | 358 | Saved 3,542 tokens, retained code counts and first file samples |
| `rtk test /tmp/axon-tsc-trace.sh` | 3,900 | 139 | Shows only last 5 lines plus tee pointer; much smaller but omits grouped files/codes |

`rtk tsc /tmp/axon-tsc-trace.sh` was also checked. In this environment it invoked the TypeScript tool path and emitted only `TypeScript compilation completed` plus a tee pointer because TypeScript is not installed for this repo, so `rtk test` is the executable RTK wrapper comparison for the controlled trace.

## Fidelity Checks

- Unit tests verify `tsc`, `typescript`, and `compiler` aliases normalize to the `tsc` command family.
- Unit tests verify diagnostics are grouped by file and summarized by TS code counts.
- Unit tests verify indented continuation lines are retained in the diagnostic summary.
- Unit tests verify malformed compiler-like input passes through unchanged.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- Test-output, package-manager, linter, and log filtering were added later on 2026-07-01; see `docs/evidence/shell-filter-test-2026-07-01.md`, `docs/evidence/shell-filter-package-2026-07-01.md`, `docs/evidence/shell-filter-lint-2026-07-01.md`, and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Shell-filter CCR recovery was added later on 2026-07-01; see `docs/evidence/shell-filter-ccr-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
