# Shell Filter Grep Evidence - 2026-07-01

## Scope

Change validated in this note:

- `axon filter grep --budget=N` handles `rg`/grep-style `path:line:match` output.
- Matches are grouped by file, long lines are truncated, and omitted per-file matches are summarized.
- The filter retries with stricter per-file caps and then clamps as a last resort so output stays within the requested token budget.
- Malformed, unsafe, small, or non-beneficial inputs pass through unchanged or through the existing safe fallback path.
- The native grep filter was compared against RTK on a real repo search.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
ctest --test-dir build --output-on-failure -R test_shell_filter
rg -n "compression|telemetry|artifact|filter" . --glob '!third_party/**' --glob '!build/**' --glob '!editors/vscode/node_modules/**' > /tmp/axon-grep-raw.txt
./build/axon filter grep --budget=600 < /tmp/axon-grep-raw.txt > /tmp/axon-grep-filtered.txt 2> /tmp/axon-grep-filtered.metrics
rtk grep "compression|telemetry|artifact|filter" . --glob '!third_party/**' --glob '!build/**' --glob '!editors/vscode/node_modules/**' > /tmp/axon-grep-rtk.txt
```

Token estimates use Axon's existing `(bytes + 3) / 4` estimator.

## Results

| Case | Input Tokens | Output Tokens | Result |
|------|--------------|---------------|--------|
| Raw `rg` trace | 243,745 | 243,745 | 477 matches in 34 files |
| `axon filter grep --budget=600` | 243,745 | 600 | Saved 243,145 tokens, stayed within budget |
| `rtk grep ...` | 243,745 | 3,799 | Saved 239,946 tokens for this payload |

Axon emitted:

```text
[axon filter] kind=plain_text input_tokens=243745 output_tokens=600 saved=243145 changed=true
```

## Fidelity Checks

- Unit tests verify grouped output includes the global summary, per-file headers, and omitted-match summaries.
- Unit tests verify very long match lines include a `[truncated]` marker.
- Unit tests verify malformed grep-like input uses safe passthrough/fallback behavior.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- JSON, TypeScript compiler, test-output, package-manager, linter, and log filtering were added later on 2026-07-01; see `docs/evidence/shell-filter-json-2026-07-01.md`, `docs/evidence/shell-filter-tsc-2026-07-01.md`, `docs/evidence/shell-filter-test-2026-07-01.md`, `docs/evidence/shell-filter-package-2026-07-01.md`, `docs/evidence/shell-filter-lint-2026-07-01.md`, and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
