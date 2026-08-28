#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
drain="$root/scripts/maintenance/axon-queue-drain.sh"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
project="$temp/project"
mkdir -p "$project/.axon" "$temp/bin" "$temp/locks"
touch "$project/.axon/index.duckdb"
printf 'a.md\n' > "$project/.axon/pending-writes.txt"
# shellcheck disable=SC2016 # The fake binary must receive runtime variables literally.
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'cat >/dev/null' \
  'if [ -n "${AXON_TEST_ACTIVE_DIR:-}" ]; then' \
  '  mkdir "$AXON_TEST_ACTIVE_DIR" 2>/dev/null || touch "${AXON_TEST_ACTIVE_DIR}.overlap"' \
  '  trap '\''rmdir "$AXON_TEST_ACTIVE_DIR" 2>/dev/null || true'\'' EXIT' \
  '  sleep 0.1' \
  'fi' \
  ': > "$PWD/.axon/pending-writes.txt"' > "$temp/bin/axon"
chmod +x "$temp/bin/axon"

AXON_BIN="$temp/bin/axon" AXON_QUEUE_MAX_AGE_SECONDS=99999 bash "$drain" "$project"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 1

key=$(printf '%s' "$project" | cksum | awk '{print $1}')
lock_dir="$temp/locks/axon-queue-drain-${key}.lock"
mkdir -p "$lock_dir/owner-$$-live-owner"
TMPDIR="$temp/locks" AXON_BIN="$temp/bin/axon" AXON_QUEUE_MAX_AGE_SECONDS=0 bash "$drain" "$project"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 1
rm -rf "$lock_dir"

# A live owner must never be expired merely because its lock is old.
mkdir -p "$lock_dir/owner-$$-old-live-owner"
python3 - "$lock_dir" <<'PY'
import os, sys, time
old = time.time() - 3600
os.utime(sys.argv[1], (old, old))
PY
TMPDIR="$temp/locks" AXON_BIN="$temp/bin/axon" AXON_QUEUE_MAX_AGE_SECONDS=0 bash "$drain" "$project"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 1
rm -rf "$lock_dir"

# A worker killed before its EXIT trap must not block this project forever.
mkdir -p "$lock_dir/owner-2147483647-dead-owner"
TMPDIR="$temp/locks" AXON_BIN="$temp/bin/axon" AXON_QUEUE_MAX_AGE_SECONDS=0 bash "$drain" "$project"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 0
test ! -e "$lock_dir"

# Many contenders reclaiming one dead owner must remain mutually exclusive.
printf 'race.md\n' > "$project/.axon/pending-writes.txt"
mkdir -p "$lock_dir/owner-2147483647-dead-race"
pids=""
for _ in $(seq 1 24); do
  TMPDIR="$temp/locks" AXON_BIN="$temp/bin/axon" AXON_TEST_ACTIVE_DIR="$temp/active-drain" \
    AXON_QUEUE_MAX_AGE_SECONDS=0 bash "$drain" "$project" &
  pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
test ! -e "$temp/active-drain.overlap"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 0
test ! -e "$lock_dir"
echo "queue_drain_ok=true"
