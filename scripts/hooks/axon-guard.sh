#!/usr/bin/env bash
# axon-guard — PreToolUse hook
# Bloqueia Grep e Glob quando o índice axon existe, forçando uso das ferramentas MCP.
#
# Instalar via: axon/scripts/install.sh
# Configurado em: <project>/.claude/settings.json com matcher "Grep" e "Glob"

if ! command -v jq &>/dev/null; then
  exit 0
fi

AXON_DB="$PWD/.axon/index.duckdb"

# Pass-through: projeto atual não foi indexado pelo axon
[ ! -f "$AXON_DB" ] && exit 0

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
    exit 0
    ;;
esac

jq -n \
  --arg reason "$REASON" \
  '{
    "hookSpecificOutput": {
      "hookEventName": "PreToolUse",
      "permissionDecision": "deny",
      "permissionDecisionReason": $reason
    }
  }'
