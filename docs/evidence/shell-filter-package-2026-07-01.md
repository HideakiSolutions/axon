# Shell Filter Package Evidence - 2026-07-01

## Scope

- `axon filter package --budget=N` parses package-manager output from stdin.
- `npm`, `pnpm`, `yarn`, and `bun` command labels normalize to the `package` filter family.
- Repeated package operation lines such as `add <name> <version>` are counted and capped.
- Install summaries, funding/audit/vulnerability lines, script lines, warnings, and errors are retained.
- Non-package-like input passes through safely instead of emitting a lossy package summary.

This note does not claim parity for every RTK command family.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
npm --prefix editors/vscode install --dry-run --ignore-scripts > /tmp/axon-npm-install-dry-run.txt 2>&1 || true
./build/axon filter npm --budget=300 < /tmp/axon-npm-install-dry-run.txt > /tmp/axon-npm-install-filtered.txt 2> /tmp/axon-npm-install-filtered.err
rtk npm --prefix editors/vscode install --dry-run --ignore-scripts > /tmp/axon-npm-install-rtk.txt 2> /tmp/axon-npm-install-rtk.err || true
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

Real npm dry-run trace from `editors/vscode/package-lock.json`:

| Payload | Input tokens | Output tokens | Notes |
| --- | ---: | ---: | --- |
| Raw `npm --prefix editors/vscode install --dry-run --ignore-scripts` | 1,113 | 1,113 | Baseline, 189 repeated `add` lines |
| `axon filter npm --budget=300` | 1,113 | 176 | Saved 937 tokens, retained install summary and package samples |
| `rtk npm --prefix editors/vscode install --dry-run --ignore-scripts` | 1,113 | 1,112 | Returned essentially the raw dry-run output in this case |

Axon stderr:

```text
[axon filter] kind=plain_text input_tokens=1113 output_tokens=176 saved=937 changed=true
```

## Fidelity Checks

- Unit tests verify npm install-style output is grouped, counted, and capped.
- Unit tests verify script/error lines such as `npm ERR!` and `sh: 1: tsc: not found` are retained.
- Unit tests verify `npm`, `pnpm`, and `yarn` labels normalize to the `package` command family.
- Unit tests verify malformed non-package-like input passes through unchanged.
- Unit tests verify tight budgets are respected.

## Remaining Gaps

- Linter and log filtering were added later on 2026-07-01; see `docs/evidence/shell-filter-lint-2026-07-01.md` and `docs/evidence/shell-filter-log-2026-07-01.md`.
- Shell-filter CCR recovery was added later on 2026-07-01; see `docs/evidence/shell-filter-ccr-2026-07-01.md`.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
