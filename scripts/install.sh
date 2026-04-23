#!/usr/bin/env bash
# axon install — configura um projeto para usar o axon context engine
#
# Uso: ./scripts/install.sh [project-path]
# Default: diretório atual
#
# O que instala:
#   1. ~/.claude/hooks/axon-guard.sh       — hook PreToolUse global (bloqueia Grep/Glob)
#   2. ~/.claude/hooks/axon-auto-index.sh  — hook UserPromptSubmit (sweep horário de deletados)
#   3. ~/.claude/hooks/axon-post-edit.sh   — hook PostToolUse (write-through síncrono após Write/Edit)
#   4. ~/.claude/hooks/axon-build-guard.sh — hook PreToolUse (bloqueia make/ninja com -j alto)
#   5. <project>/.claude/CLAUDE.md         — instruções de uso do axon
#   6. <project>/.claude/settings.json     — registra hooks para Grep, Glob, Bash (build), UserPromptSubmit, PostToolUse

set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AXON_BIN="$(dirname "$SCRIPTS_DIR")/build/axon"
AXON_LIB="$(dirname "$SCRIPTS_DIR")/third_party/duckdb/lib"

PROJECT="${1:-$(pwd)}"
PROJECT="$(realpath "$PROJECT")"

HOOKS_DIR="$HOME/.claude/hooks"
CLAUDE_DIR="$PROJECT/.claude"

echo "[axon] Installing for: $PROJECT"

# Verificar dependências
if ! command -v jq &>/dev/null; then
  echo "[axon] ERROR: jq não encontrado. Instale: sudo apt install jq" >&2
  exit 1
fi

# 1. Instalar hooks globais
mkdir -p "$HOOKS_DIR"
cp "$SCRIPTS_DIR/hooks/axon-guard.sh" "$HOOKS_DIR/axon-guard.sh"
chmod +x "$HOOKS_DIR/axon-guard.sh"
echo "[axon] ✓ Hook guard: $HOOKS_DIR/axon-guard.sh"

cp "$SCRIPTS_DIR/hooks/axon-auto-index.sh" "$HOOKS_DIR/axon-auto-index.sh"
chmod +x "$HOOKS_DIR/axon-auto-index.sh"
echo "[axon] ✓ Hook auto-index: $HOOKS_DIR/axon-auto-index.sh"

cp "$SCRIPTS_DIR/hooks/axon-post-edit.sh" "$HOOKS_DIR/axon-post-edit.sh"
chmod +x "$HOOKS_DIR/axon-post-edit.sh"
echo "[axon] ✓ Hook post-edit: $HOOKS_DIR/axon-post-edit.sh"

cp "$SCRIPTS_DIR/hooks/axon-build-guard.sh" "$HOOKS_DIR/axon-build-guard.sh"
chmod +x "$HOOKS_DIR/axon-build-guard.sh"
echo "[axon] ✓ Hook build-guard: $HOOKS_DIR/axon-build-guard.sh"

# 2. Criar diretório .claude do projeto
mkdir -p "$CLAUDE_DIR"

# 3. Instalar CLAUDE.md
cp "$SCRIPTS_DIR/templates/CLAUDE.md" "$CLAUDE_DIR/CLAUDE.md"
echo "[axon] ✓ CLAUDE.md: $CLAUDE_DIR/CLAUDE.md"

# 4. Criar/atualizar settings.json com hooks para Grep e Glob
SETTINGS="$CLAUDE_DIR/settings.json"
HOOK_CMD="bash $HOOKS_DIR/axon-guard.sh"

AUTO_INDEX_CMD="bash $HOOKS_DIR/axon-auto-index.sh"
POST_EDIT_CMD="bash $HOOKS_DIR/axon-post-edit.sh"
BUILD_GUARD_CMD="bash $HOOKS_DIR/axon-build-guard.sh"

GREP_HOOK=$(jq -n --arg cmd "$HOOK_CMD" '{
  "matcher": "Grep",
  "hooks": [{"type": "command", "command": $cmd}]
}')

GLOB_HOOK=$(jq -n --arg cmd "$HOOK_CMD" '{
  "matcher": "Glob",
  "hooks": [{"type": "command", "command": $cmd}]
}')

BUILD_GUARD_HOOK=$(jq -n --arg cmd "$BUILD_GUARD_CMD" '{
  "matcher": "Bash",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 5}]
}')

AUTO_INDEX_HOOK=$(jq -n --arg cmd "$AUTO_INDEX_CMD" '{
  "matcher": "",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 5}]
}')

POST_EDIT_HOOK=$(jq -n --arg cmd "$POST_EDIT_CMD" '{
  "matcher": "Write|Edit|MultiEdit|NotebookEdit|Bash",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 10}]
}')

if [ ! -f "$SETTINGS" ]; then
  jq -n \
    --argjson grep "$GREP_HOOK" \
    --argjson glob "$GLOB_HOOK" \
    --argjson buildguard "$BUILD_GUARD_HOOK" \
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '{"hooks": {"PreToolUse": [$grep, $glob, $buildguard], "UserPromptSubmit": [$autoindex], "PostToolUse": [$postedit]}}' > "$SETTINGS"
else
  # Merge: adiciona entradas axon evitando duplicatas
  EXISTING=$(cat "$SETTINGS")
  MERGED=$(echo "$EXISTING" | jq \
    --argjson grep "$GREP_HOOK" \
    --argjson glob "$GLOB_HOOK" \
    --argjson buildguard "$BUILD_GUARD_HOOK" \
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '
      .hooks.PreToolUse //= [] |
      .hooks.PreToolUse |= (
        [.[] | select(
          .matcher != "Grep" and
          .matcher != "Glob" and
          (.hooks[0].command | contains("axon-build-guard") | not)
        )] +
        [$grep, $glob, $buildguard]
      ) |
      .hooks.UserPromptSubmit //= [] |
      .hooks.UserPromptSubmit |= (
        [.[] | select(.hooks[0].command | contains("axon-auto-index") | not)] +
        [$autoindex]
      ) |
      .hooks.PostToolUse //= [] |
      .hooks.PostToolUse |= (
        [.[] | select(.hooks[0].command | contains("axon-post-edit") | not)] +
        [$postedit]
      )
    ')
  echo "$MERGED" > "$SETTINGS"
fi

echo "[axon] ✓ Settings: $SETTINGS"

# 5. Indexar o projeto
if [ -f "$AXON_BIN" ]; then
  echo "[axon] Indexando projeto (pode levar alguns instantes)..."
  LD_LIBRARY_PATH="$AXON_LIB" "$AXON_BIN" index "$PROJECT"
  echo "[axon] ✓ Indexação concluída"
else
  echo "[axon] AVISO: binário não encontrado em $AXON_BIN — indexe manualmente com: axon index $PROJECT"
fi

echo ""
echo "[axon] Instalação concluída. Reinicie o Claude Code para ativar os hooks."
