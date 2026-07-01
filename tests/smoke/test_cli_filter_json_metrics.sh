#!/usr/bin/env bash
set -euo pipefail

axon_bin="${1:?axon binary path required}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

raw="$tmpdir/raw.txt"
for file in $(seq 0 7); do
  for line in $(seq 1 12); do
    printf 'src/module%s.cpp:%s: matched filter metric payload payload payload payload payload\n' "$file" "$line" >> "$raw"
  done
done

"$axon_bin" filter grep --budget=260 --metrics=json < "$raw" > "$tmpdir/filtered.txt" 2> "$tmpdir/metrics.json"
grep -q '"type":"axon_filter_metrics"' "$tmpdir/metrics.json"
grep -q '"command":"grep"' "$tmpdir/metrics.json"
grep -q '"layer":"shell_filtering"' "$tmpdir/metrics.json"
grep -q '"changed":true' "$tmpdir/metrics.json"
grep -q '"recoverable":true' "$tmpdir/metrics.json"
grep -Eq '"ccr_artifact_id":"ccr_[a-f0-9]+"' "$tmpdir/metrics.json"
grep -Eq '"input_tokens":[1-9][0-9]*' "$tmpdir/metrics.json"
grep -Eq '"output_tokens":[0-9]+' "$tmpdir/metrics.json"
grep -Eq '"tokens_saved":[1-9][0-9]*' "$tmpdir/metrics.json"

artifact_id="$(sed -n 's/.*"ccr_artifact_id":"\([^"]*\)".*/\1/p' "$tmpdir/metrics.json")"
"$axon_bin" artifact-retrieve "$artifact_id" > "$tmpdir/recovered.txt"
cmp -s "$raw" "$tmpdir/recovered.txt"

"$axon_bin" filter grep --budget=260 --json-metrics < "$raw" > "$tmpdir/alias-filtered.txt" 2> "$tmpdir/alias.json"
grep -q '"type":"axon_filter_metrics"' "$tmpdir/alias.json"

"$axon_bin" filter grep --budget=260 < "$raw" > "$tmpdir/human-filtered.txt" 2> "$tmpdir/human.err"
grep -q '^\[axon filter\]' "$tmpdir/human.err"
if grep -q '"type":"axon_filter_metrics"' "$tmpdir/human.err"; then
  echo "default metrics unexpectedly used JSON" >&2
  exit 1
fi
