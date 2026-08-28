#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
hook="$root/scripts/hooks/axon-post-edit.sh"
command -v jq >/dev/null 2>&1 || exit 0

temp="$(mktemp -d)"
temp="$(cd "$temp" && pwd -P)"
trap 'rm -rf "$temp"' EXIT
project="$temp/project"
foreign="$temp/foreign"
mkdir -p "$project/.axon" "$foreign"
touch "$project/owned.md" "$project/space ü.md" "$foreign/foreign.md"
# shellcheck disable=SC2016 # The fake drain script must receive $1 literally.
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$1" >> %q\n' "$temp/drains" > "$temp/drain"
chmod +x "$temp/drain"

run_hook() {
  local file="$1"
  (cd "$project" && jq -n --arg f "$file" '{tool_name:"Write",tool_input:{file_path:$f}}' | AXON_QUEUE_DRAIN_SCRIPT="$temp/drain" bash "$hook")
}

run_hook "$project/owned.md"
grep -Fx "$project/owned.md" "$project/.axon/pending-writes.txt" >/dev/null
run_hook "$project/space ü.md"
grep -Fx "$project/space ü.md" "$project/.axon/pending-writes.txt" >/dev/null

run_hook "$foreign/foreign.md"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 2
for _ in $(seq 1 20); do [ -f "$temp/drains" ] && break; sleep 0.05; done
grep -Fx "$project" "$temp/drains" >/dev/null

# Concurrent hook processes must serialize without losing or duplicating paths.
pids=""
for i in $(seq 1 24); do
  touch "$project/concurrent-$i.md"
  (cd "$project" && jq -n --arg f "$project/concurrent-$i.md" '{tool_name:"Write",tool_input:{file_path:$f}}' | AXON_QUEUE_DRAIN_SCRIPT=/nonexistent bash "$hook") &
  pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 26
test "$(sort -u "$project/.axon/pending-writes.txt" | wc -l)" -eq 26
test ! -e "$project/.axon/pending-writes.txt.lock.d"

# A bounded lock timeout preserves eventual consistency through sync-requested.
mkdir -p "$project/.axon/pending-writes.txt.lock.d/owner-$$-live-owner"
touch "$project/timeout.md"
(cd "$project" && jq -n --arg f "$project/timeout.md" '{tool_name:"Write",tool_input:{file_path:$f}}' | AXON_QUEUE_LOCK_WAIT_ATTEMPTS=1 AXON_QUEUE_DRAIN_SCRIPT=/nonexistent bash "$hook")
test -f "$project/.axon/sync-requested"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 26
rm -rf "$project/.axon/pending-writes.txt.lock.d"
echo "post_edit_project_boundary_ok=true"
