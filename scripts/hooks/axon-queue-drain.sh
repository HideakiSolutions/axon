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
ATTEMPT_TIMEOUT_SECONDS="${AXON_QUEUE_ATTEMPT_TIMEOUT_SECONDS:-30}"
case "$ATTEMPT_TIMEOUT_SECONDS" in
  ''|*[!0-9]*|0) ATTEMPT_TIMEOUT_SECONDS=30 ;;
esac

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
lock_token="$$-$(date +%s)-${RANDOM:-0}"
lock_dir="${TMPDIR:-/tmp}/axon-queue-drain-${key}.lock"
lock_owner="$lock_dir/owner-$$-$lock_token"

# Called indirectly by signal/EXIT traps.
# shellcheck disable=SC2317
release_lock() {
  rmdir "$lock_owner" 2>/dev/null || true
  rmdir "$lock_dir" 2>/dev/null || true
}

acquire_lock() {
  if mkdir "$lock_dir" 2>/dev/null; then
    mkdir "$lock_owner"
    return 0
  fi

  local existing_owner="" owner_name="" owner_pid=""
  existing_owner=$(find "$lock_dir" -mindepth 1 -maxdepth 1 -type d -name 'owner-*' -print 2>/dev/null | head -n 1)
  # An empty directory is the atomic mkdir/owner handoff window. It is never
  # reclaimed: this avoids confusing a newly-created lock with an orphan.
  [ -n "$existing_owner" ] || return 1
  owner_name=${existing_owner##*/}
  owner_pid=${owner_name#owner-}
  owner_pid=${owner_pid%%-*}
  case "$owner_pid" in ''|*[!0-9]*) owner_pid="" ;; esac
  if [ -n "$owner_pid" ] && kill -0 "$owner_pid" 2>/dev/null; then
    return 1
  fi

  # The exact owner is a non-empty child directory. Only one reclaimer can
  # remove it; losers stop before touching the canonical directory. A cleanup
  # can never remove a successor because rmdir refuses its non-empty lock.
  rmdir "$existing_owner" 2>/dev/null || return 1
  rmdir "$lock_dir" 2>/dev/null || return 1
  if mkdir "$lock_dir" 2>/dev/null; then
    mkdir "$lock_owner"
    return 0
  fi
  return 1
}

acquire_lock || { echo '[axon-queue-drain] already running' >&2; exit 0; }
trap release_lock EXIT
trap 'release_lock; exit 130' INT
trap 'release_lock; exit 143' TERM
request='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_overview","arguments":{"limit":1}}}'
framed_request=$(printf 'Content-Length: %s\r\n\r\n%s' "${#request}" "$request")

run_bounded_attempt() {
  local child_pid watchdog_pid status
  (cd "$ROOT" && exec "$AXON_BIN" serve) <<<"$framed_request" >/dev/null 2>/dev/null &
  child_pid=$!
  (
    remaining=$ATTEMPT_TIMEOUT_SECONDS
    while [ "$remaining" -gt 0 ] && kill -0 "$child_pid" 2>/dev/null; do
      sleep 1
      remaining=$((remaining - 1))
    done
    kill -0 "$child_pid" 2>/dev/null || exit 0
    kill -TERM "$child_pid" 2>/dev/null || exit 0
    sleep 1
    kill -KILL "$child_pid" 2>/dev/null || true
  ) &
  watchdog_pid=$!
  set +e
  wait "$child_pid"
  status=$?
  set -e
  wait "$watchdog_pid" 2>/dev/null || true
  return "$status"
}

for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  if run_bounded_attempt; then
    remaining=$(grep -cve '^\s*$' "$QUEUE" 2>/dev/null || true)
    if [[ "$remaining" -eq 0 ]]; then
      echo "[axon-queue-drain] drained lines=$count attempt=$attempt" >&2
      exit 0
    fi
  fi
  echo "[axon-queue-drain] attempt_failed attempt=$attempt timeout_seconds=$ATTEMPT_TIMEOUT_SECONDS" >&2
  sleep "$attempt"
done
echo "[axon-queue-drain] degraded: drain failed lines=$count attempts=$MAX_ATTEMPTS" >&2
exit 2
