#!/usr/bin/env bash
# axon-auto-index — UserPromptSubmit hook
# Janitor horário: sinaliza que o próximo tool call do MCP server deve fazer
# sync completo (walk + BLAKE3-skip + prune) — pega edições externas que não
# passaram por hooks (vim/IDE, git pull, scripts).
#
# Não chamamos o CLI aqui: o `axon serve` segura lock exclusivo no DuckDB, então
# ferramentas CLI concorrentes falham. Em vez disso tocamos .axon/sync-requested
# — o MCP server detecta no drain e roda sync em-processo.
#
# Zero tokens consumidos: nenhum output no stdout em condições normais.
#
# Instalar via: axon/scripts/install.sh

# shellcheck disable=SC1091
[ -f "$(dirname "${BASH_SOURCE[0]}")/_log.sh" ] && . "$(dirname "${BASH_SOURCE[0]}")/_log.sh"
log() { command -v axon_log &>/dev/null && axon_log "axon-auto-index" "$@"; }

# Guard: executar no máximo uma vez por hora por projeto
SESSION_FLAG="/tmp/axon-indexed-$(date +%Y%m%d%H)-${PWD##*/}"
[ -f "$SESSION_FLAG" ] && exit 0
touch "$SESSION_FLAG"

[ ! -d "$PWD/.axon" ] && { log "skip" '{"reason":"not-axon-project"}'; exit 0; }

# Marca sync como pendente; MCP server processa no próximo drain
touch "$PWD/.axon/sync-requested" 2>/dev/null || true
log "sync-requested" '{}'

exit 0
