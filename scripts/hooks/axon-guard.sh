#!/usr/bin/env bash
# axon-guard — PreToolUse hook
# Bloqueia Grep e Glob quando o índice axon existe, forçando uso das ferramentas MCP.
#
# Instalar via: axon/scripts/install.sh
# Configurado em: <project>/.claude/settings.json com matcher "Grep" e "Glob"

# Optional structured logging — silent if helper is missing.
# shellcheck disable=SC1091
[ -f "$(dirname "${BASH_SOURCE[0]}")/_log.sh" ] && . "$(dirname "${BASH_SOURCE[0]}")/_log.sh"
log() { command -v axon_log &>/dev/null && axon_log "axon-guard" "$@"; }

if ! command -v jq &>/dev/null; then
  exit 0
fi

AXON_DB="$PWD/.axon/index.duckdb"

# Pass-through: projeto atual não foi indexado pelo axon
[ ! -f "$AXON_DB" ] && { log "pass" '{"reason":"no-index"}'; exit 0; }

INPUT=$(cat)
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

case "$TOOL" in
  Grep)
    REASON="Use axon em vez de Grep: chame get_context_capsule(query) para busca semântica com 76-98% menos tokens. Se o projeto ainda não foi indexado, chame run_pipeline() primeiro."
    ;;
  Glob)
    REASON="Use axon em vez de Glob: chame get_skeleton(files) para assinaturas de arquivos ou get_context_capsule(query) para contexto semântico. Se o projeto ainda não foi indexado, chame run_pipeline() primeiro."
    ;;
  *)
    log "pass" "$(jq -n --arg t "$TOOL" '{tool:$t}')"
    exit 0
    ;;
esac

log "deny" "$(jq -n --arg t "$TOOL" '{tool:$t}')"

jq -n \
  --arg reason "$REASON" \
  '{
    "hookSpecificOutput": {
      "hookEventName": "PreToolUse",
      "permissionDecision": "deny",
      "permissionDecisionReason": $reason
    }
  }'
