#!/usr/bin/env bash
# axon-post-edit — PostToolUse hook
# Write-through após Write/Edit/MultiEdit/NotebookEdit e detecção de
# mudanças de filesystem via Bash (rm, mv, git checkout, etc).
#
# Estratégia: o MCP server (axon serve) segura lock exclusivo no DuckDB,
# então NÃO podemos abrir o DB em paralelo via CLI. Em vez disso, os hooks
# só escrevem markers no filesystem (.axon/pending-writes.txt e
# .axon/sync-requested). O MCP server drena essas sinalizações antes de
# cada tool call, mantendo write-through síncrono do ponto de vista do
# Claude sem conflito de lock (single-writer).
#
# Instalar via: axon/scripts/install.sh

AXON_DIR="$PWD/.axon"
QUEUE="$AXON_DIR/pending-writes.txt"
SYNC_MARKER="$AXON_DIR/sync-requested"

# Pass-through se o projeto não for indexado pelo axon
[ ! -d "$AXON_DIR" ] && exit 0
command -v jq &>/dev/null || exit 0

INPUT=$(cat)
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

case "$TOOL" in
  Write|Edit|MultiEdit)
    FILE=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
    [ -z "$FILE" ] && exit 0
    # Append atômico — flock evita corrida entre hooks concorrentes
    (
      flock -x 9
      echo "$FILE" >> "$QUEUE"
    ) 9> "$QUEUE.lock" 2>/dev/null || true
    ;;

  NotebookEdit)
    FILE=$(echo "$INPUT" | jq -r '.tool_input.notebook_path // empty')
    [ -z "$FILE" ] && exit 0
    (
      flock -x 9
      echo "$FILE" >> "$QUEUE"
    ) 9> "$QUEUE.lock" 2>/dev/null || true
    ;;

  Bash)
    # Não parseamos o comando (alias, subshell, heredoc, pipe fariam o parse
    # frágil e com false negatives). Marcamos sync — o server faz walk +
    # BLAKE3 skip + prune no próximo tool call. Custo amortizado via
    # BLAKE3 para arquivos não-mudados.
    touch "$SYNC_MARKER" 2>/dev/null || true
    ;;

  *)
    exit 0
    ;;
esac

exit 0
