#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
router="$root/scripts/maintenance/route_pending_writes.py"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT

python_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
  else
    printf '%s\n' "$1"
  fi
}

one="$temp/projects/one"
two="$temp/projects/two"
global="$temp/global"
mkdir -p "$one/.axon" "$two/.axon" "$global/.axon"
touch "$one/a.md" "$two/b.md" "$global/local.md" "$temp/outside.md"
one_entry="$(python_path "$one/a.md")"
two_entry="$(python_path "$two/b.md")"
global_entry="$(python_path "$global/local.md")"
outside_entry="$(python_path "$temp/outside.md")"
queue_arg="$(python_path "$global/.axon/pending-writes.txt")"
registry_arg="$(python_path "$global/registry.json")"
printf '%s\n%s\n%s\n%s\n' "$one_entry" "$two_entry" "$global_entry" "$outside_entry" > "$global/.axon/pending-writes.txt"
python3 - "$registry_arg" "$(python_path "$one")" "$(python_path "$two")" "$(python_path "$global")" <<'PY'
import json
import sys
from pathlib import Path

Path(sys.argv[1]).write_text(
    json.dumps({"repos": [{"root": root} for root in sys.argv[2:]]}),
    encoding="utf-8",
)
PY

dry="$(python3 "$router" --queue "$queue_arg" --registry "$registry_arg")"
echo "$dry" | python3 -c 'import json,sys; x=json.load(sys.stdin); assert x["mode"] == "dry_run" and x["routed_entries"] == 3 and x["unmatched_entries"] == 1'
test ! -f "$one/.axon/pending-writes.txt"

python3 "$router" --apply --quarantine-unmatched --queue "$queue_arg" --registry "$registry_arg" >/dev/null
grep -Fx "$one_entry" "$one/.axon/pending-writes.txt" >/dev/null
grep -Fx "$two_entry" "$two/.axon/pending-writes.txt" >/dev/null
grep -Fx "$global_entry" "$global/.axon/pending-writes.txt" >/dev/null
if grep -Fx "$outside_entry" "$global/.axon/pending-writes.txt" >/dev/null; then
  echo "unmatched entry remained active" >&2
  exit 1
fi
find "$global/.axon" -name 'pending-writes.unroutable-*.txt' -exec grep -Fx "$outside_entry" {} \; | grep -q .
find "$global/.axon" -name 'pending-writes.routed-*.bak' | grep -q .

# Recovery is conservative: only an existing path that belongs to a registered
# project returns to a queue; an outside path stays as quarantine evidence.
quarantine="$global/.axon/pending-writes.unroutable-999.txt"
printf '%s\n%s\n' "$one_entry" "$outside_entry" > "$quarantine"
recovery="$(python3 "$router" --queue "$queue_arg" --registry "$registry_arg" --apply)"
echo "$recovery" | python3 -c 'import json,sys; x=json.load(sys.stdin); r=x["quarantine_recovery"]; assert r["recovered_entries"] == 1 and r["retained_entries"] >= 1'
grep -Fx "$one_entry" "$one/.axon/pending-writes.txt" >/dev/null
grep -Fx "$outside_entry" "$quarantine" >/dev/null

# After the approved retention period, only nonexistent paths may expire. The
# original evidence is retained as a dated backup.
stale="$global/.axon/pending-writes.unroutable-200.txt"
printf '%s\n' "$temp/expired.md" > "$stale"
python3 - "$(python_path "$stale")" <<'PY'
import os
import sys
import time

stamp = time.time() - (31 * 86_400)
os.utime(sys.argv[1], (stamp, stamp))
PY
pruned="$(python3 "$router" --queue "$queue_arg" --registry "$registry_arg" --apply)"
echo "$pruned" | python3 -c 'import json,sys; x=json.load(sys.stdin); assert x["quarantine_retention"]["expired_entries"] == 1'
test ! -e "$stale"
find "$global/.axon" -name 'pending-writes.unroutable-200.pruned-*.bak' | grep -q .
echo "route_pending_writes_ok=true"
