#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runner="$root/scripts/maintenance/axon-queue-drain-registry.sh"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
project="$temp/project"
mkdir -p "$project/.axon" "$temp/bin"
touch "$project/.axon/index.duckdb"
printf 'a.md\n' > "$project/.axon/pending-writes.txt"
# shellcheck disable=SC2016 # The fake binary must receive $PWD literally.
printf '%s\n' '#!/usr/bin/env bash' 'cat >/dev/null' ': > "$PWD/.axon/pending-writes.txt"' > "$temp/bin/axon"
chmod +x "$temp/bin/axon"

if AXON_BIN="$temp/bin/axon" bash "$runner" >/dev/null 2>&1; then
  echo 'runner accepted an implicit global sweep' >&2
  exit 1
fi

AXON_BIN="$temp/bin/axon" AXON_QUEUE_MAX_AGE_SECONDS=0 bash "$runner" "$project"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 0
echo "queue_drain_registry_scope_ok=true"
