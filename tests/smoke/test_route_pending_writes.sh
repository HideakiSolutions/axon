#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
router="$root/scripts/maintenance/route_pending_writes.py"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT

one="$temp/projects/one"
two="$temp/projects/two"
global="$temp/global"
mkdir -p "$one/.axon" "$two/.axon" "$global/.axon"
touch "$one/a.md" "$two/b.md" "$global/local.md" "$temp/outside.md"
printf '%s\n%s\n%s\n%s\n' "$one/a.md" "$two/b.md" "$global/local.md" "$temp/outside.md" > "$global/.axon/pending-writes.txt"
printf '{"repos":[{"root":"%s"},{"root":"%s"},{"root":"%s"}]}' "$one" "$two" "$global" > "$global/registry.json"

dry="$(python3 "$router" --queue "$global/.axon/pending-writes.txt" --registry "$global/registry.json")"
echo "$dry" | python3 -c 'import json,sys; x=json.load(sys.stdin); assert x["mode"] == "dry_run" and x["routed_entries"] == 3 and x["unmatched_entries"] == 1'
test ! -f "$one/.axon/pending-writes.txt"

python3 "$router" --apply --quarantine-unmatched --queue "$global/.axon/pending-writes.txt" --registry "$global/registry.json" >/dev/null
grep -Fx "$one/a.md" "$one/.axon/pending-writes.txt" >/dev/null
grep -Fx "$two/b.md" "$two/.axon/pending-writes.txt" >/dev/null
grep -Fx "$global/local.md" "$global/.axon/pending-writes.txt" >/dev/null
if grep -Fx "$temp/outside.md" "$global/.axon/pending-writes.txt" >/dev/null; then
  echo "unmatched entry remained active" >&2
  exit 1
fi
find "$global/.axon" -name 'pending-writes.unroutable-*.txt' -exec grep -Fx "$temp/outside.md" {} \; | grep -q .
find "$global/.axon" -name 'pending-writes.routed-*.bak' | grep -q .

# Recovery is conservative: only an existing path that belongs to a registered
# project returns to a queue; an outside path stays as quarantine evidence.
quarantine="$global/.axon/pending-writes.unroutable-999.txt"
printf '%s\n%s\n' "$one/a.md" "$temp/outside.md" > "$quarantine"
recovery="$(python3 "$router" --queue "$global/.axon/pending-writes.txt" --registry "$global/registry.json" --apply)"
echo "$recovery" | python3 -c 'import json,sys; x=json.load(sys.stdin); r=x["quarantine_recovery"]; assert r["recovered_entries"] == 1 and r["retained_entries"] >= 1'
grep -Fx "$one/a.md" "$one/.axon/pending-writes.txt" >/dev/null
grep -Fx "$temp/outside.md" "$quarantine" >/dev/null

# After the approved retention period, only nonexistent paths may expire. The
# original evidence is retained as a dated backup.
stale="$global/.axon/pending-writes.unroutable-200.txt"
printf '%s\n' "$temp/expired.md" > "$stale"
touch -d '31 days ago' "$stale"
pruned="$(python3 "$router" --queue "$global/.axon/pending-writes.txt" --registry "$global/registry.json" --apply)"
echo "$pruned" | python3 -c 'import json,sys; x=json.load(sys.stdin); assert x["quarantine_retention"]["expired_entries"] == 1'
test ! -e "$stale"
find "$global/.axon" -name 'pending-writes.unroutable-200.pruned-*.bak' | grep -q .
echo "route_pending_writes_ok=true"
