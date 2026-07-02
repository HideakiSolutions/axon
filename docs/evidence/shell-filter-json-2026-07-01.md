# Shell Filter JSON Evidence - 2026-07-01

## Scope

- `axon filter json --budget=N` parses JSON output and emits a schema/shape summary.
- The filter strips raw string/number/boolean values, keeps object keys, array sizes, and nested type summaries.
- Malformed JSON passes through safely instead of emitting a lossy parse guess.
- The native JSON filter was compared against RTK's `json --schema` mode on a real repo lockfile.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
./build/axon filter json --budget=600 < editors/vscode/package-lock.json > /tmp/axon-json-filter.txt 2> /tmp/axon-json-filter.err
rtk json --schema editors/vscode/package-lock.json > /tmp/rtk-json-schema.txt 2> /tmp/rtk-json-schema.err
wc -c editors/vscode/package-lock.json /tmp/axon-json-filter.txt /tmp/rtk-json-schema.txt
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw `editors/vscode/package-lock.json` | 26,029 | 26,029 | Baseline, 104,113 bytes |
| `axon filter json --budget=600` | 26,029 | 376 | Saved 25,653 tokens, stayed within budget |
| `rtk json --schema editors/vscode/package-lock.json` | 26,029 | 1,468 | Saved 24,561 tokens for this payload |

Axon stderr:

```text
[axon filter] kind=json input_tokens=26029 output_tokens=376 saved=25653 changed=true
```

## Fidelity Checks

- Unit tests verify JSON output is classified as `json`, reduced, and contains schema markers such as `packages: array`.
- Unit tests verify raw string values are not repeated in schema summaries.
- Unit tests verify malformed JSON remains unchanged.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- TypeScript compiler, test-output, package-manager, linter, and log filtering were added later on 2026-07-01; see `docs/evidence/shell-filter-tsc-2026-07-01.md`, `docs/evidence/shell-filter-test-2026-07-01.md`, `docs/evidence/shell-filter-package-2026-07-01.md`, `docs/evidence/shell-filter-lint-2026-07-01.md`, and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Shell-filter CCR recovery was added later on 2026-07-01; see `docs/evidence/shell-filter-ccr-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
