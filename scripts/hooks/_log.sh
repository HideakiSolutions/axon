#!/usr/bin/env bash
# _log.sh — shared structured-logging helper for axon hooks.
#
# Sourced (not exec'd) by axon-guard.sh, axon-build-guard.sh,
# axon-auto-index.sh, axon-post-edit.sh. Provides one function:
#
#   axon_log <hook-name> <decision> [<extra-json>]
#
# Writes one JSON line per invocation to .axon/logs/<hook-name>.jsonl
# under the project's .axon/ if it exists, else to ~/.axon/logs/. The
# logger is intentionally best-effort — never blocks the hook's exit
# path, never errors out of the parent shell. Also rotates the live
# log if it exceeds AXON_LOG_MAX_BYTES (default 5 MiB) by gzipping it
# to <hook>.<epoch>.jsonl.gz.

# Source-guard: re-sourcing should be a no-op.
[ -n "${__AXON_LOG_SOURCED:-}" ] && return 0
__AXON_LOG_SOURCED=1

AXON_LOG_MAX_BYTES="${AXON_LOG_MAX_BYTES:-5242880}"

axon_log() {
  local hook="$1"
  local decision="$2"
  local extra="${3:-{\}}"

  # Pick the log dir: project-local .axon/logs/ when in a project, else
  # ~/.axon/logs/ as a fallback.
  local log_dir
  if [ -d "${PWD}/.axon" ]; then
    log_dir="${PWD}/.axon/logs"
  else
    log_dir="${HOME}/.axon/logs"
  fi
  mkdir -p "$log_dir" 2>/dev/null || return 0

  local log_file="${log_dir}/${hook}.jsonl"

  # Rotate if the live file exceeds the cap. stat -c (Linux) and stat -f
  # (BSD/macOS) take different format flags; one of them succeeds.
  if [ -f "$log_file" ]; then
    local size
    size=$(stat -c%s "$log_file" 2>/dev/null || stat -f%z "$log_file" 2>/dev/null || echo 0)
    if [ "${size:-0}" -gt "$AXON_LOG_MAX_BYTES" ]; then
      local rotated
      rotated="${log_dir}/${hook}.$(date +%s).jsonl"
      mv -f "$log_file" "$rotated" 2>/dev/null || true
      if command -v gzip &>/dev/null; then
        gzip -f "$rotated" 2>/dev/null || true
      fi
    fi
  fi

  # Compose the line. We avoid jq here so this helper has zero hard
  # dependencies — printf's %s with the extra field assumed pre-shaped.
  local ts
  ts="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
  printf '{"ts":"%s","hook":"%s","decision":"%s","extra":%s}\n' \
    "$ts" "$hook" "$decision" "$extra" >> "$log_file" 2>/dev/null || true
}
