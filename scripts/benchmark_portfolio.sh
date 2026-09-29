#!/usr/bin/env bash
set -euo pipefail

# Measures real E2E catalog projection and candidate evaluation.  It deliberately reports wall
# time only; no synthetic latency or throughput figures are inferred by this runner.
build_dir=${1:-/tmp/axon-g12-build}
catalog_test="$build_dir/tests/test_portfolio_capability_catalog"
evaluator="$build_dir/portfolio_eval"
dataset="$(cd "$(dirname "$0")/.." && pwd)/evals/portfolio/truth-set-v1.json"

test -x "$catalog_test"
test -x "$evaluator"

measure() {
  local label=$1
  shift
  local started ended
  started=$(date +%s%N)
  "$@" >/dev/null
  ended=$(date +%s%N)
  printf '%s_ms=%s\n' "$label" "$(( (ended - started) / 1000000 ))"
}

printf 'benchmark_schema=axon/portfolio-benchmark/v1\n'
printf 'hardware=%s\n' "$(uname -srm)"
measure catalog_e2e "$catalog_test"
measure candidate_eval python3 "$(dirname "$dataset")/run_portfolio_eval.py" --evaluator "$evaluator" --dataset "$dataset"
