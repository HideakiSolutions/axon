#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT/scripts/hooks/axon-auto-index.sh"
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT

one="$TEMP/one/cambio-real"
two="$TEMP/two/cambio-real"
mkdir -p "$one/.axon" "$two/.axon"

(cd "$one" && bash "$SCRIPT")
(cd "$two" && bash "$SCRIPT")

test -f "$one/.axon/sync-requested"
test -f "$two/.axon/sync-requested"
echo "auto-index project key test: passed"
