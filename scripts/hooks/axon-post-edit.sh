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

PROJECT_ROOT="$(cd "$PWD" && pwd -P)"
AXON_DIR="$PROJECT_ROOT/.axon"
QUEUE="$AXON_DIR/pending-writes.txt"
SYNC_MARKER="$AXON_DIR/sync-requested"
DRAIN_SCRIPT="${AXON_QUEUE_DRAIN_SCRIPT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/axon-queue-drain.sh}"
# Cap queue at 1MiB. Above this, rotate the file out of the way and request a
# full sync — the MCP server's BLAKE3-skip walk reconciles equivalent state
# while keeping the live queue bounded even if the server has been idle.
QUEUE_MAX_BYTES=1048576

# shellcheck disable=SC1091
[ -f "$(dirname "${BASH_SOURCE[0]}")/_log.sh" ] && . "$(dirname "${BASH_SOURCE[0]}")/_log.sh"
log() { command -v axon_log &>/dev/null && axon_log "axon-post-edit" "$@"; }

# Pass-through se o projeto não for indexado pelo axon
[ ! -d "$AXON_DIR" ] && exit 0
command -v jq &>/dev/null || exit 0

# Append a path to the queue under flock, rotating if it has grown unbounded.
# Triggered by Write/Edit/MultiEdit/NotebookEdit handlers below.
append_to_queue() {
  local file="$1"
  (
    flock -x 9
    if [ -f "$QUEUE" ]; then
      local size
      size=$(stat -c%s "$QUEUE" 2>/dev/null || stat -f%z "$QUEUE" 2>/dev/null || echo 0)
      if [ "${size:-0}" -gt "$QUEUE_MAX_BYTES" ]; then
        mv -f "$QUEUE" "$AXON_DIR/pending-writes.$(date +%s).bak" 2>/dev/null || true
        touch "$SYNC_MARKER" 2>/dev/null || true
      fi
    fi
    printf '%s\n' "$file" >> "$QUEUE"
  ) 9> "$QUEUE.lock" 2>/dev/null || true
}

# A queue belongs to exactly one indexed project. A hook may be invoked from a
# broad workspace (for example $HOME); never enqueue a path from another repo
# into that project's drain queue.
queue_owned_file() {
  local candidate="$1"
  case "$candidate" in
    /*) ;;
    *) candidate="$PROJECT_ROOT/$candidate" ;;
  esac
  local parent base resolved
  parent="$(dirname "$candidate")"
  base="$(basename "$candidate")"
  resolved="$(cd "$parent" 2>/dev/null && pwd -P)/$base"
  case "$resolved" in
    "$PROJECT_ROOT"/*)
      append_to_queue "$resolved"
      printf '%s' "$resolved"
      return 0
      ;;
    *)
      log "skip-foreign-path" "$(jq -n --arg f "$candidate" '{file:$f,reason:"outside-project-root"}')"
      return 1
      ;;
  esac
}

# The normal path is still write-through on the next MCP tool call. This is a
# bounded fallback for an idle client: it returns immediately below the age and
# size thresholds, serializes per project in the worker, and never scans the
# global registry.
request_async_drain() {
  [[ -x "$DRAIN_SCRIPT" ]] || return 0
  (bash "$DRAIN_SCRIPT" "$PROJECT_ROOT" </dev/null >/dev/null 2>&1 &)
}

INPUT=$(cat)
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

case "$TOOL" in
  Write|Edit|MultiEdit)
    FILE=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
    [ -z "$FILE" ] && exit 0
    QUEUED="$(queue_owned_file "$FILE")" || exit 0
    log "queued" "$(jq -n --arg t "$TOOL" --arg f "$QUEUED" '{tool:$t,file:$f}')"
    ;;

  NotebookEdit)
    FILE=$(echo "$INPUT" | jq -r '.tool_input.notebook_path // empty')
    [ -z "$FILE" ] && exit 0
    QUEUED="$(queue_owned_file "$FILE")" || exit 0
    log "queued" "$(jq -n --arg f "$QUEUED" '{tool:"NotebookEdit",file:$f}')"
    ;;

  Bash)
    # Não parseamos o comando (alias, subshell, heredoc, pipe fariam o parse
    # frágil e com false negatives). Marcamos sync — o server faz walk +
    # BLAKE3 skip + prune no próximo tool call. Custo amortizado via
    # BLAKE3 para arquivos não-mudados.
    touch "$SYNC_MARKER" 2>/dev/null || true
    log "sync-requested" '{"tool":"Bash"}'
    ;;

  *)
    exit 0
    ;;
esac

request_async_drain

exit 0
