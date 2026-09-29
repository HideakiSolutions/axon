# Shell Filter Lint Evidence - 2026-07-01

## Scope

- `axon filter lint --budget=N` parses linter output from stdin.
- `eslint`, `ruff`, `prettier`, and `format` command labels normalize to the `lint` filter family.
- Ruff concise diagnostics shaped like `file:line:column: CODE message` are grouped by file and code.
- ESLint stylish diagnostics are grouped by file and rule.
- Summary lines such as `Found N errors` and problem totals are retained.
- Non-lint-like input passes through safely instead of emitting a lossy lint summary.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
ruff check --output-format=concise /tmp/axon_lint_fixture.py > /tmp/axon-ruff-fixture-raw.txt 2>&1 || true
./build/axon filter ruff --budget=90 < /tmp/axon-ruff-fixture-raw.txt > /tmp/axon-ruff-fixture-filtered-90.txt 2> /tmp/axon-ruff-fixture-filtered-90.err
rtk ruff check /tmp/axon_lint_fixture.py > /tmp/axon-ruff-fixture-rtk.txt 2> /tmp/axon-ruff-fixture-rtk.err || true
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

Real Ruff output from a temporary Python lint fixture:

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw `ruff check --output-format=concise /tmp/axon_lint_fixture.py` | 1,401 | 1,401 | Baseline, 81 diagnostics in 1 file |
| `axon filter ruff --budget=90` | 1,401 | 88 | Saved 1,313 tokens, retained code counts and first diagnostic samples |
| `rtk ruff check /tmp/axon_lint_fixture.py` | 1,401 | 81 | Smaller, retained top rules/files but no concrete line samples |

Axon stderr:

```text
[axon filter] kind=plain_text input_tokens=1401 output_tokens=88 saved=1313 changed=true
```

## Fidelity Checks

- Unit tests verify Ruff concise diagnostics are grouped by file and rule code.
- Unit tests verify ESLint stylish diagnostics are parsed and grouped.
- Unit tests verify `ruff`, `eslint`, `prettier`, and `format` labels normalize to the `lint` command family.
- Unit tests verify malformed non-lint-like input passes through unchanged.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- Log filtering was added later on 2026-07-01; see `docs/evidence/shell-filter-log-2026-07-01.md`.
- Shell-filter CCR recovery was added later on 2026-07-01; see `docs/evidence/shell-filter-ccr-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
