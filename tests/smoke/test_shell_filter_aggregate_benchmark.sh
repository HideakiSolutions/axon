#!/usr/bin/env bash
set -euo pipefail

axon_bin="$(realpath "${1:?axon binary path required}")"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v jq >/dev/null 2>&1; then
  echo "SKIP: jq is required for aggregate shell-filter benchmark"
  exit 0
fi

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

(
  cd "$repo_root"
  AXON_BIN="$axon_bin" bash scripts/benchmark_shell_filters.sh "$out_dir" > "$out_dir/report.out"
)

jsonl="$out_dir/shell-filter-benchmark.jsonl"
test -s "$jsonl"

case_count="$(jq -s 'length' "$jsonl")"
if [[ "$case_count" != "8" ]]; then
  echo "expected 8 shell-filter benchmark cases, got $case_count" >&2
  cat "$jsonl" >&2
  exit 1
fi

jq -e 'select(.axon_tokens_saved <= 0 or .axon_output_tokens > .budget)' "$jsonl" >/tmp/axon-shell-benchmark-bad.json && {
  cat /tmp/axon-shell-benchmark-bad.json >&2
  exit 1
} || true

jq -e 'select(.ccr_recovered != "true")' "$jsonl" >/tmp/axon-shell-benchmark-ccr-bad.json && {
  cat /tmp/axon-shell-benchmark-ccr-bad.json >&2
  exit 1
} || true

grep -q '^| diff ' "$out_dir/shell-filter-benchmark.md"
grep -q '^| log ' "$out_dir/shell-filter-benchmark.md"

echo "shell_filter_aggregate_benchmark_ok=true"
