#!/usr/bin/env bash
# axon-shell-guard — PreToolUse hook
# Blocks known noisy raw Bash output in indexed projects and routes agents to
# Axon MCP retrieval or `axon filter` with CCR/metrics.
#
# Escape hatch for intentional raw output:
#   AXON_ALLOW_RAW_SHELL=1 <command>
#
# Configured in: <project>/.claude/settings.json with matcher "Bash"

# shellcheck disable=SC1091
[ -f "$(dirname "${BASH_SOURCE[0]}")/_log.sh" ] && . "$(dirname "${BASH_SOURCE[0]}")/_log.sh"
log() { command -v axon_log &>/dev/null && axon_log "axon-shell-guard" "$@"; }

command -v jq &>/dev/null || exit 0

AXON_DB="$PWD/.axon/index.duckdb"
[ ! -f "$AXON_DB" ] && { log "pass" '{"reason":"no-index"}'; exit 0; }

INPUT=$(cat)
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')
[ "$TOOL" = "Bash" ] || exit 0

CMD=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
[ -z "$CMD" ] && exit 0

matches() {
  printf '%s\n' "$CMD" | grep -Eq -- "$1"
}

deny() {
  local family="$1"
  local suggestion="$2"
  local reason
  reason="[axon] Raw Bash output for '$family' bypasses Axon metrics, CCR recovery, and token budgets. Use: $suggestion. Escape for intentional raw output: AXON_ALLOW_RAW_SHELL=1 <command>."
  log "deny" "$(jq -n --arg family "$family" '{family:$family}')"
  jq -n --arg reason "$reason" '{
    "hookSpecificOutput": {
      "hookEventName": "PreToolUse",
      "permissionDecision": "deny",
      "permissionDecisionReason": $reason
    }
  }'
  exit 0
}

if [ "${AXON_ALLOW_RAW_SHELL:-0}" = "1" ] || matches '(^|[[:space:];&|])AXON_ALLOW_RAW_SHELL=1([[:space:]]|$)'; then
  log "pass" '{"reason":"escape-hatch"}'
  exit 0
fi

# Already routed through Axon or an explicit compatibility fallback.
if matches '(^|[[:space:];&|])axon[[:space:]]+filter([[:space:]]|$)' ||
   matches '(^|[[:space:];&|])rtk([[:space:]]|$)'; then
  log "pass" '{"reason":"filtered"}'
  exit 0
fi

# Commands that do not stream stdout back to the agent are outside this hook's
# token-output boundary. Follow-up reads are still covered by retrieval guidance.
if matches '(^|[[:space:]])1?>[[:space:]]*[^&]'; then
  log "pass" '{"reason":"stdout-redirected"}'
  exit 0
fi

token='(^|[[:space:];&|`()])'
end='([[:space:];&|`)]|$)'

if matches "${token}git[[:space:]]+diff${end}" &&
   ! matches '(^|[[:space:]])--(stat|name-only|name-status|quiet|check)([[:space:]]|$)'; then
  deny "diff" "git diff ... | axon filter diff --budget=600 --metrics=json"
fi

if matches "${token}(rg|grep|ack|ag)${end}"; then
  deny "grep" "get_context_capsule(query=...) for code search, or <search command> | axon filter grep --budget=600 --metrics=json"
fi

if matches "${token}(cat|sed|awk|nl)${end}" &&
   matches '\.(c|cc|cpp|cxx|h|hh|hpp|ts|tsx|js|jsx|py|rs|go|java|cs|php|dart|kt|kts|vue|lua|nix|rb|swift|scala|sh|bash|json|md)([[:space:];&|`)]|$)'; then
  deny "raw-file-read" "get_skeleton(files=[...]) or get_context_capsule(query=..., pivot_files=[...]) before raw reads"
fi

if matches "${token}(pytest|vitest|ctest|gtest|cargo[[:space:]]+test|go[[:space:]]+test|npm[[:space:]]+test|pnpm[[:space:]]+test|yarn[[:space:]]+test|bun[[:space:]]+test|mvn[[:space:]]+test|gradle[[:space:]]+test)${end}"; then
  deny "test" "<test command> 2>&1 | axon filter test --budget=700 --metrics=json"
fi

if matches "${token}(tsc|vue-tsc)${end}"; then
  deny "tsc" "<tsc command> 2>&1 | axon filter tsc --budget=500 --metrics=json"
fi

if matches "${token}(eslint|ruff|prettier|flake8|mypy|pylint|clippy)${end}" ||
   matches "${token}cargo[[:space:]]+clippy${end}"; then
  deny "lint" "<lint command> 2>&1 | axon filter lint --budget=500 --metrics=json"
fi

if matches "${token}(npm[[:space:]]+(install|ci)|pnpm[[:space:]]+install|yarn[[:space:]]+install|bun[[:space:]]+install)${end}"; then
  deny "package" "<package command> 2>&1 | axon filter package --budget=350 --metrics=json"
fi

if matches "${token}(journalctl|docker[[:space:]]+logs|kubectl[[:space:]]+logs)${end}" ||
   matches "${token}tail[[:space:]]+(-f|-n[[:space:]]+[0-9]{3,})"; then
  deny "log" "<log command> 2>&1 | axon filter log --budget=700 --metrics=json"
fi

log "pass" "$(jq -n --arg reason "not-noisy" '{reason:$reason}')"
exit 0
