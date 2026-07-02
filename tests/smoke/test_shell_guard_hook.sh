#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
hook="$repo_root/scripts/hooks/axon-shell-guard.sh"

if ! command -v jq >/dev/null 2>&1; then
  echo "SKIP: jq is required for shell guard hook smoke"
  exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/.axon"
touch "$tmpdir/.axon/index.duckdb"

run_hook() {
  local command="$1"
  (
    cd "$tmpdir"
    jq -n --arg cmd "$command" '{tool_name:"Bash",tool_input:{command:$cmd}}' | bash "$hook"
  )
}

out="$(run_hook 'git diff -- src/core/capsule.cpp')"
echo "$out" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
echo "$out" | jq -e '.hookSpecificOutput.permissionDecisionReason | contains("axon filter diff")' >/dev/null

out="$(run_hook 'rg -n "source_ref" src tests')"
echo "$out" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
echo "$out" | jq -e '.hookSpecificOutput.permissionDecisionReason | contains("axon filter grep")' >/dev/null

out="$(run_hook 'pytest tests 2>&1')"
echo "$out" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
echo "$out" | jq -e '.hookSpecificOutput.permissionDecisionReason | contains("axon filter test")' >/dev/null

out="$(run_hook 'sed -n "1,240p" src/core/capsule.cpp')"
echo "$out" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
echo "$out" | jq -e '.hookSpecificOutput.permissionDecisionReason | contains("get_context_capsule")' >/dev/null

if [[ -n "$(run_hook 'git diff -- src | axon filter diff --budget=600 --metrics=json')" ]]; then
  echo "filtered command unexpectedly denied" >&2
  exit 1
fi

if [[ -n "$(run_hook 'AXON_ALLOW_RAW_SHELL=1 rg -n source_ref src')" ]]; then
  echo "escape-hatch command unexpectedly denied" >&2
  exit 1
fi

rm -f "$tmpdir/.axon/index.duckdb"
if [[ -n "$(run_hook 'rg -n source_ref src')" ]]; then
  echo "non-indexed project unexpectedly denied" >&2
  exit 1
fi

echo "shell_guard_hook_ok=true"
