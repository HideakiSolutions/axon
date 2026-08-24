#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
hook="$root/scripts/hooks/axon-post-edit.sh"
command -v jq >/dev/null 2>&1 || exit 0

temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
project="$temp/project"
foreign="$temp/foreign"
mkdir -p "$project/.axon" "$foreign"
touch "$project/owned.md" "$foreign/foreign.md"
# shellcheck disable=SC2016 # The fake drain script must receive $1 literally.
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$1" >> %q\n' "$temp/drains" > "$temp/drain"
chmod +x "$temp/drain"

run_hook() {
  local file="$1"
  (cd "$project" && jq -n --arg f "$file" '{tool_name:"Write",tool_input:{file_path:$f}}' | AXON_QUEUE_DRAIN_SCRIPT="$temp/drain" bash "$hook")
}

run_hook "$project/owned.md"
grep -Fx "$project/owned.md" "$project/.axon/pending-writes.txt" >/dev/null

run_hook "$foreign/foreign.md"
test "$(wc -l < "$project/.axon/pending-writes.txt")" -eq 1
for _ in $(seq 1 20); do [ -f "$temp/drains" ] && break; sleep 0.05; done
grep -Fx "$project" "$temp/drains" >/dev/null
echo "post_edit_project_boundary_ok=true"
