#!/usr/bin/env bash
# axon-build-guard — PreToolUse hook
# Bloqueia builds que consomem recursos demais do host (make -j alto, -j ilimitado, nproc).
# Limite padrão: 2 jobs. Motivo: deixar CPU livre para MCP server axon, embeddings llama.cpp
# e outras sessões concorrentes (second-brain, outros Claudes).
#
# Escape hatch: defina AXON_ALLOW_HIGH_PARALLELISM=1 no ambiente antes do comando.
#
# Configurado em: <project>/.claude/settings.json com matcher "Bash"

if ! command -v jq &>/dev/null; then
  exit 0
fi

if [ "${AXON_ALLOW_HIGH_PARALLELISM:-0}" = "1" ]; then
  exit 0
fi

INPUT=$(cat)
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')
[ "$TOOL" = "Bash" ] || exit 0

CMD=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
[ -z "$CMD" ] && exit 0

MAX_JOBS=2

deny() {
  jq -n --arg reason "$1" '{
    "hookSpecificOutput": {
      "hookEventName": "PreToolUse",
      "permissionDecision": "deny",
      "permissionDecisionReason": $reason
    }
  }'
  exit 0
}

# Detecta make/cmake/ninja em qualquer posição do comando (após &&, |, etc.)
echo "$CMD" | grep -Eq '(^|[[:space:];&|`(])(make|cmake[[:space:]]+--build|ninja)([[:space:]]|$)' || exit 0

REASON_BASE="Build em paralelismo alto trava o host (MCP axon, embeddings e outras sessões competem por CPU). Use 'make -j${MAX_JOBS}' (default do projeto). Escape: export AXON_ALLOW_HIGH_PARALLELISM=1 antes do comando. Veja .claude/CLAUDE.md > Build constraints."

# -j$(nproc), -j`nproc`, -j${nproc}
if echo "$CMD" | grep -Eq -- '-j[[:space:]]*[`$]'; then
  deny "$REASON_BASE Detectado: -j com expansão dinâmica (\$(nproc) ou similar)."
fi

# -j N (com número explícito)
JOBS=$(echo "$CMD" | grep -oE -- '-j[[:space:]]*[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
if [ -n "$JOBS" ] && [ "$JOBS" -gt "$MAX_JOBS" ]; then
  deny "$REASON_BASE Detectado: -j${JOBS} (máximo permitido: -j${MAX_JOBS})."
fi

# -j sozinho (make interpreta como ilimitado)
# Match -j seguido por espaço+não-número, fim de linha, ou separador de comando
if echo "$CMD" | grep -Eq -- '-j([[:space:]]+[^0-9]|[[:space:]]*$|[[:space:]]*[;&|])'; then
  deny "$REASON_BASE Detectado: -j sem número (make interpreta como ilimitado)."
fi

# ninja sem -j explícito também é ilimitado por default
if echo "$CMD" | grep -Eq '(^|[[:space:];&|`(])ninja([[:space:]]|$)' && ! echo "$CMD" | grep -Eq -- '-j[[:space:]]*[0-9]+'; then
  deny "$REASON_BASE Detectado: ninja sem -j explícito (default é ilimitado)."
fi

exit 0
