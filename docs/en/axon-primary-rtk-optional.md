# Axon-First Context and Shell Filtering

Use this guide when Axon is the primary local context layer for an agent and RTK remains optional fallback tooling.

## Target Setup

```json
{
  "mcpServers": {
    "axon": {
      "command": "/path/to/axon/build/axon",
      "args": ["serve"]
    }
  }
}
```

Index the project before the agent session:

```bash
axon init .
axon index --force
```

For richer capsules, enable symbol granularity and reindex:

```toml
granularity = "symbol"
token_budget = 8000
capsule_compression = "body"
```

## Agent Routing Rules

Use Axon before raw reads:

| Task | Primary Axon path |
| --- | --- |
| Understand a repo | `get_overview` then `get_context_capsule` |
| Work on a focused change | `get_context_capsule` with a query and explicit `token_budget` |
| Expand a capsule file | Use the returned `expand_command` |
| Inspect signatures | `get_skeleton` |
| Trace impact | `get_impact_graph`, `get_callers`, `get_tests_for` |
| Recover lossy content | `artifact_retrieve` or `axon artifact-retrieve <id>` |

Capsule file entries include `source_ref` and `expand_command`. Treat those as the first expansion path. Raw file reads are a fallback only when the Axon expansion is insufficient.

## Shell Output Filtering

Pipe large command output through Axon filters:

```bash
some-command 2>&1 | axon filter auto --budget=800 --metrics=json
rg -n "symbol" src | axon filter grep --budget=600 --metrics=json
npm test 2>&1 | axon filter test --budget=700 --metrics=json
```

Installed Claude Code projects include `axon-shell-guard.sh`, a Bash `PreToolUse` hook that denies known noisy raw commands in indexed repos unless they are routed through `axon filter`, RTK compatibility fallback, stdout redirection, or an explicit `AXON_ALLOW_RAW_SHELL=1` escape hatch.

Native command families:

| Family | Aliases |
| --- | --- |
| `diff` | `git-diff` |
| `grep` | `rg` |
| `json` | `json` |
| `tsc` | `typescript`, `compiler` |
| `test` | `pytest`, `vitest`, `ctest`, `gtest` |
| `package` | `npm`, `pnpm`, `yarn`, `bun` |
| `lint` | `eslint`, `ruff`, `prettier`, `format` |
| `log` | `logs` |

Changed lossy output is recoverable by default. The emitted summary contains an `axon:ccr` marker and JSON metrics include `ccr_artifact_id` when `--metrics=json` is used.

## RTK Fallback Policy

Keep RTK installed only for compatibility or cross-checking:

- Use RTK when a command family is not yet represented by `axon filter`.
- Use RTK to compare reduction quality during benchmark work.
- Do not route normal context retrieval through RTK when `get_context_capsule`, `get_skeleton`, or `expand_command` can answer first.

Run the aggregate comparison before release or after changing shell filters:

```bash
bash scripts/benchmark_shell_filters.sh /tmp/axon-shell-filter-bench
```

The runner validates Axon budgets, token savings, and CCR recovery for every native family, and records RTK output tokens when RTK is available.

## Release Gate

Before treating Axon as the primary layer in a release:

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
bash scripts/benchmark_shell_filters.sh /tmp/axon-shell-filter-bench
```

The CTest suite includes smoke coverage for shell JSON metrics, MCP capsule schema, docs freshness, and the aggregate shell benchmark when local dependencies are available.
