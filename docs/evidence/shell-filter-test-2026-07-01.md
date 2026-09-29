# Shell Filter Test Evidence - 2026-07-01

## Scope

- `axon filter test --budget=N` parses test-run output from stdin.
- `pytest`, `vitest`, `ctest`, and `gtest` command labels normalize to the `test` filter family.
- Pytest failure/error blocks, CTest/GTest failed-test lines, traceback hints, assertion details, and final summaries are retained.
- Non-test-like input passes through safely instead of emitting a lossy test summary.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
./build/axon filter test --budget=700 < ~/.local/share/rtk/tee/1782865275_test.log > /tmp/axon-real-test-filtered.txt 2> /tmp/axon-real-test-filtered.err
rtk test /tmp/axon-replay-pytest-trace.sh > /tmp/axon-real-test-rtk-script.txt 2> /tmp/axon-real-test-rtk-script.err || true
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

Real pytest collection-failure trace from RTK tee history:

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw `~/.local/share/rtk/tee/1782865275_test.log` | 13,346 | 13,346 | Baseline, 70 pytest collection errors |
| `axon filter test --budget=700` | 13,346 | 560 | Saved 12,786 tokens, retained first failure blocks and final summary |
| `rtk test /tmp/axon-replay-pytest-trace.sh` | 13,346 | 43 | Much smaller, but only returned the interruption summary plus a full-output tee pointer |

Axon stderr:

```text
[axon filter] kind=plain_text input_tokens=13346 output_tokens=560 saved=12786 changed=true
```

RTK output:

```text
SUMMARY:
  !!!!!!!!!!!!!!!!!!! Interrupted: 70 errors during collection !!!!!!!!!!!!!!!!!!!
  70 errors in 1.89s

[full output: ~/.local/share/rtk/tee/1782865348_test.log]
```

## Fidelity Checks

- Unit tests verify pytest collection failures keep the first failure identity, traceback hints, `ModuleNotFoundError`, and short summary.
- Unit tests verify CTest/GTest-style failures keep failed test names, source locations, and failed-count summaries.
- Unit tests verify `test`, `pytest`, `vitest`, `ctest`, and `gtest` labels normalize to the `test` command family.
- Unit tests verify malformed non-test-like input passes through unchanged.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- Package-manager, linter, and log filtering were added later on 2026-07-01; see `docs/evidence/shell-filter-package-2026-07-01.md`, `docs/evidence/shell-filter-lint-2026-07-01.md`, and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Shell-filter CCR recovery was added later on 2026-07-01; see `docs/evidence/shell-filter-ccr-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
