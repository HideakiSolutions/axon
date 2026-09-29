# Shell Guard Hook - 2026-07-01

## Scope

Reduce the remaining external-agent bypass risk by making installed Claude Code projects deny known noisy raw Bash output in indexed repositories unless the command is routed through Axon filters, RTK compatibility fallback, stdout redirection, or an explicit escape hatch.

## Implementation

- Added `scripts/hooks/axon-shell-guard.sh`.
- Wired Unix installer settings to add the hook as a Bash `PreToolUse` entry.
- Wired Windows installer settings to generate `axon-shell-guard.ps1` with equivalent command-family checks.
- Updated installed project guidance in `scripts/templates/CLAUDE.md`.
- Updated package and Axon-first migration docs.
- Added `tests/smoke/test_shell_guard_hook.sh` and registered it in CTest.

## Guarded Families

- `git diff` full output -> `axon filter diff`
- `rg`, `grep`, `ack`, `ag` -> MCP retrieval or `axon filter grep`
- raw source/doc reads via `cat`, `sed`, `awk`, `nl` -> `get_skeleton` or `get_context_capsule`
- tests -> `axon filter test`
- TypeScript diagnostics -> `axon filter tsc`
- lint diagnostics -> `axon filter lint`
- package-manager install output -> `axon filter package`
- logs -> `axon filter log`

## Safe Passthroughs

- Project has no `.axon/index.duckdb`.
- Command already uses `axon filter`.
- Command uses RTK as explicit compatibility fallback.
- Stdout is redirected away from the agent transcript.
- Command is explicitly prefixed with `AXON_ALLOW_RAW_SHELL=1`.

## Validation

```bash
bash -n scripts/hooks/axon-shell-guard.sh tests/smoke/test_shell_guard_hook.sh scripts/install.sh
bash tests/smoke/test_shell_guard_hook.sh
```

Result:

```text
shell_guard_hook_ok=true
```

The smoke test verifies denial for raw `git diff`, `rg`, `pytest`, and `sed` source reads, plus passthrough for `axon filter`, escape hatch, and non-indexed projects.
