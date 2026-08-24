#!/usr/bin/env bash
# Drain one project's Axon queue through its MCP server, with bounded retries.
set -euo pipefail

ROOT="${1:-$PWD}"
ROOT="$(cd "$ROOT" && pwd -P)"
AXON_DIR="$ROOT/.axon"
QUEUE="$AXON_DIR/pending-writes.txt"
AXON_BIN="${AXON_BIN:-axon}"
MAX_AGE_SECONDS="${AXON_QUEUE_MAX_AGE_SECONDS:-900}"
MAX_LINES="${AXON_QUEUE_MAX_LINES:-100}"
MAX_ATTEMPTS="${AXON_QUEUE_MAX_ATTEMPTS:-3}"

[[ -d "$AXON_DIR" && -f "$AXON_DIR/index.duckdb" && -f "$QUEUE" ]] || exit 0
command -v "$AXON_BIN" >/dev/null 2>&1 || { echo '[axon-queue-drain] axon binary unavailable' >&2; exit 2; }
count=$(grep -cve '^\s*$' "$QUEUE" 2>/dev/null || true)
[[ "$count" -gt 0 ]] || exit 0
mtime=$(stat -c %Y "$QUEUE" 2>/dev/null || stat -f %m "$QUEUE" 2>/dev/null || echo 0)
age=$(( $(date +%s) - mtime ))
if [[ "$count" -lt "$MAX_LINES" && "$age" -lt "$MAX_AGE_SECONDS" ]]; then
  echo "[axon-queue-drain] deferred lines=$count age_seconds=$age" >&2
  exit 0
fi

key=$(printf '%s' "$ROOT" | cksum | awk '{print $1}')
exec 9>"${TMPDIR:-/tmp}/axon-queue-drain-${key}.lock"
flock -n 9 || { echo '[axon-queue-drain] already running' >&2; exit 0; }
request='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_overview","arguments":{"limit":1}}}'

for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  if printf 'Content-Length: %s\r\n\r\n%s' "${#request}" "$request" | (cd "$ROOT" && "$AXON_BIN" serve) >/dev/null 2>/dev/null; then
    remaining=$(grep -cve '^\s*$' "$QUEUE" 2>/dev/null || true)
    if [[ "$remaining" -eq 0 ]]; then
      echo "[axon-queue-drain] drained lines=$count attempt=$attempt" >&2
      exit 0
    fi
  fi
  sleep "$attempt"
done
echo "[axon-queue-drain] degraded: drain failed lines=$count attempts=$MAX_ATTEMPTS" >&2
exit 2
