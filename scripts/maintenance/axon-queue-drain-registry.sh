#!/usr/bin/env bash
# Periodic runner: invokes the bounded per-project drainer for explicit roots.
#
# A timer must name the projects it owns.  Walking the global registry would
# cross repository ownership boundaries and can turn a local backlog into an
# unbounded batch job.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ "$#" -gt 0 ]] || {
  echo '[axon-queue-drain-registry] provide one or more project roots; refusing global registry sweep' >&2
  exit 2
}

failed=0
for requested_root in "$@"; do
  root="$(cd "$requested_root" 2>/dev/null && pwd -P)" || {
    echo "[axon-queue-drain-registry] invalid project root: $requested_root" >&2
    failed=1
    continue
  }
  [[ -d "$root/.axon" ]] || {
    echo "[axon-queue-drain-registry] not an Axon project: $root" >&2
    failed=1
    continue
  }
  if ! bash "$SCRIPT_DIR/axon-queue-drain.sh" "$root"; then failed=1; fi
done
exit "$failed"
