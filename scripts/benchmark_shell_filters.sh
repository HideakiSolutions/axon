#!/usr/bin/env bash
set -euo pipefail

axon_bin="${AXON_BIN:-./build/axon}"
out_dir="${1:-/tmp/axon-shell-filter-bench}"
lint_fixture_path=""

if [[ ! -x "$axon_bin" ]]; then
  echo "axon binary not found or not executable: $axon_bin" >&2
  exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required for benchmark metrics parsing" >&2
  exit 1
fi

mkdir -p "$out_dir"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

report="$out_dir/shell-filter-benchmark.md"
jsonl="$out_dir/shell-filter-benchmark.jsonl"
: > "$jsonl"

estimate_tokens() {
  local bytes
  bytes="$(wc -c < "$1")"
  echo $(((bytes + 3) / 4))
}

make_controlled_diff() {
  local file="$1"
  {
    echo "diff --git a/src/generated.cpp b/src/generated.cpp"
    echo "--- a/src/generated.cpp"
    echo "+++ b/src/generated.cpp"
    echo "@@ -1,160 +1,160 @@"
    for i in $(seq 1 160); do
      echo "-old line $i with verbose payload payload payload payload"
      echo "+new line $i with verbose payload payload payload payload"
    done
  } > "$file"
}

make_controlled_grep() {
  local file="$1"
  : > "$file"
  for module in $(seq 0 10); do
    for line in $(seq 1 18); do
      printf 'src/module%s.cpp:%s: matched symbol with implementation detail payload payload payload\n' "$module" "$((line + 10))" >> "$file"
    done
  done
}

make_controlled_json() {
  local file="$1"
  {
    echo '{"packages":['
    for i in $(seq 0 120); do
      printf '{"name":"pkg-%s","version":"1.2.%s","dependencies":{"duckdb":"^1","tree-sitter":"^0.22"}}' "$i" "$i"
      [[ "$i" == "120" ]] || printf ','
      printf '\n'
    done
    echo '],"metadata":{"project":"axon","secret":"do-not-repeat"}}'
  } > "$file"
}

make_controlled_tsc() {
  local file="$1"
  : > "$file"
  for module in $(seq 0 11); do
    for idx in $(seq 1 9); do
      printf 'editors/vscode/src/module%s.ts(%s,%s): error TS2322: Type mismatch in generated fixture with payload payload payload %s\n' "$module" "$((idx + 20))" "$((idx + 4))" "$idx" >> "$file"
    done
  done
}

make_controlled_test() {
  local file="$1"
  {
    echo "==================================== ERRORS ===================================="
    for i in $(seq 0 34); do
      echo "________ ERROR collecting tests/unit/test_module_$i.py ________"
      echo "ImportError while importing test module '/repo/tests/unit/test_module_$i.py'."
      echo "Traceback:"
      echo "tests/unit/test_module_$i.py:6: in <module>"
      echo "E   ModuleNotFoundError: No module named 'agent_runtime'"
    done
    echo "=========================== short test summary info ============================"
    for i in $(seq 0 34); do echo "ERROR tests/unit/test_module_$i.py"; done
    echo "35 errors in 1.89s"
  } > "$file"
}

make_controlled_package() {
  local file="$1"
  : > "$file"
  for i in $(seq 0 180); do
    printf 'add package-%s 1.2.%s\n' "$i" "$i" >> "$file"
  done
  {
    echo "added 181 packages in 318ms"
    echo "50 packages are looking for funding"
    echo "found 0 vulnerabilities"
  } >> "$file"
}

make_controlled_lint() {
  local file="$1"
  : > "$file"
  for module in $(seq 0 10); do
    for idx in $(seq 1 9); do
      printf 'src/module_%s.py:%s:%s: F401 lint message with repeated explanatory payload payload %s\n' "$module" "$((idx + 10))" "$idx" "$idx" >> "$file"
    done
  done
  echo "Found 99 errors." >> "$file"
}

make_controlled_log() {
  local file="$1"
  : > "$file"
  for i in $(seq 0 90); do
    printf 'Jul 01 10:%02d:01 host systemd[1234]: Started app-com.example-%s.scope.\n' "$((i % 60))" "$i" >> "$file"
  done
  for i in $(seq 0 14); do
    printf 'Jul 01 10:40:%02d host gpg-agent[900]: cannot connect to daemon: IPC connect call failed\n' "$i" >> "$file"
  done
  echo "Jul 01 10:55:01 host app[500]: FATAL unable to open artifact store" >> "$file"
}

prepare_traces() {
  diff_raw="$tmpdir/diff.raw"
  git diff -- . ':!third_party' ':!build' > "$diff_raw" || true
  [[ -s "$diff_raw" ]] || make_controlled_diff "$diff_raw"

  grep_raw="$tmpdir/grep.raw"
  rg -n "filter|compression|telemetry|artifact" src tests docs README.md > "$grep_raw" || true
  [[ -s "$grep_raw" ]] || make_controlled_grep "$grep_raw"

  json_raw="$tmpdir/json.raw"
  if [[ -f editors/vscode/package-lock.json ]]; then
    cp editors/vscode/package-lock.json "$json_raw"
  else
    make_controlled_json "$json_raw"
  fi

  tsc_raw="$tmpdir/tsc.raw"
  if [[ -f "${HOME:-}/.local/share/rtk/tee/1782438637_tsc.log" ]]; then
    cp "${HOME}/.local/share/rtk/tee/1782438637_tsc.log" "$tsc_raw"
  else
    make_controlled_tsc "$tsc_raw"
  fi

  test_raw="$tmpdir/test.raw"
  if [[ -f "${HOME:-}/.local/share/rtk/tee/1782865275_test.log" ]]; then
    cp "${HOME}/.local/share/rtk/tee/1782865275_test.log" "$test_raw"
  else
    make_controlled_test "$test_raw"
  fi

  package_raw="$tmpdir/package.raw"
  if [[ -d editors/vscode && "$(command -v npm || true)" ]]; then
    npm --prefix editors/vscode install --dry-run --ignore-scripts > "$package_raw" 2>&1 || true
  fi
  [[ -s "$package_raw" ]] || make_controlled_package "$package_raw"

  lint_raw="$tmpdir/lint.raw"
  if command -v ruff >/dev/null 2>&1; then
    lint_fixture_path="$tmpdir/axon_lint_fixture.py"
    for i in $(seq 1 80); do echo "import os  # unused $i" >> "$lint_fixture_path"; done
    ruff check --output-format=concise "$lint_fixture_path" > "$lint_raw" 2>&1 || true
  fi
  [[ -s "$lint_raw" ]] || make_controlled_lint "$lint_raw"

  log_raw="$tmpdir/log.raw"
  journalctl -n 200 --no-pager > "$log_raw" 2>/dev/null || true
  [[ -s "$log_raw" ]] || make_controlled_log "$log_raw"
}

run_rtk() {
  local name="$1" raw="$2" out="$3" err="$4"
  if ! command -v rtk >/dev/null 2>&1; then
    return 2
  fi

  case "$name" in
    diff) rtk diff - < "$raw" > "$out" 2> "$err" || true ;;
    grep) rtk grep "filter|compression|telemetry|artifact" . --glob '!third_party/**' --glob '!build/**' > "$out" 2> "$err" || true ;;
    json) rtk json --schema "$raw" > "$out" 2> "$err" || true ;;
    log) rtk log "$raw" > "$out" 2> "$err" || true ;;
    package)
      if [[ -d editors/vscode ]]; then
        rtk npm --prefix editors/vscode install --dry-run --ignore-scripts > "$out" 2> "$err" || true
      else
        return 2
      fi
      ;;
    lint)
      if [[ -n "$lint_fixture_path" && -f "$lint_fixture_path" ]]; then
        rtk ruff check "$lint_fixture_path" > "$out" 2> "$err" || true
      else
        return 2
      fi
      ;;
    test|tsc)
      local replay="$tmpdir/rtk-replay-$name.sh"
      printf '#!/usr/bin/env bash\ncat %q\n' "$raw" > "$replay"
      chmod +x "$replay"
      rtk test "$replay" > "$out" 2> "$err" || true
      ;;
    *)
      return 2
      ;;
  esac
}

run_case() {
  local name="$1" command="$2" budget="$3" raw="$4"
  local case_dir="$out_dir/$name"
  mkdir -p "$case_dir"
  cp "$raw" "$case_dir/raw.txt"

  local axon_out="$case_dir/axon.txt"
  local axon_metrics="$case_dir/axon.metrics.json"
  local start end elapsed_ms
  start="$(date +%s%3N)"
  "$axon_bin" filter "$command" --budget="$budget" --metrics=json < "$raw" > "$axon_out" 2> "$axon_metrics"
  end="$(date +%s%3N)"
  elapsed_ms=$((end - start))

  local input_tokens output_tokens saved changed recoverable artifact_id
  input_tokens="$(jq -r '.input_tokens' "$axon_metrics")"
  output_tokens="$(jq -r '.output_tokens' "$axon_metrics")"
  saved="$(jq -r '.tokens_saved' "$axon_metrics")"
  changed="$(jq -r '.changed' "$axon_metrics")"
  recoverable="$(jq -r '.recoverable' "$axon_metrics")"
  artifact_id="$(jq -r '.ccr_artifact_id // ""' "$axon_metrics")"

  if [[ "$changed" != "true" || "$saved" -le 0 || "$output_tokens" -gt "$budget" ]]; then
    echo "benchmark case failed Axon validation: $name" >&2
    cat "$axon_metrics" >&2
    exit 1
  fi

  local recovered="n/a"
  if [[ "$recoverable" == "true" && -n "$artifact_id" ]]; then
    "$axon_bin" artifact-retrieve "$artifact_id" > "$case_dir/recovered.txt"
    if cmp -s "$raw" "$case_dir/recovered.txt"; then
      recovered="true"
    else
      echo "CCR recovery mismatch for $name" >&2
      exit 1
    fi
  fi

  local rtk_tokens="" rtk_status="not_run"
  if run_rtk "$name" "$raw" "$case_dir/rtk.txt" "$case_dir/rtk.err"; then
    if [[ -s "$case_dir/rtk.txt" ]]; then
      rtk_tokens="$(estimate_tokens "$case_dir/rtk.txt")"
      rtk_status="ok"
    else
      rtk_tokens="0"
      rtk_status="empty"
    fi
  else
    rtk_status="unsupported_or_missing"
  fi

  jq -n \
    --arg name "$name" \
    --arg command "$command" \
    --argjson budget "$budget" \
    --argjson input_tokens "$input_tokens" \
    --argjson output_tokens "$output_tokens" \
    --argjson saved "$saved" \
    --argjson elapsed_ms "$elapsed_ms" \
    --arg recovered "$recovered" \
    --arg rtk_status "$rtk_status" \
    --arg rtk_tokens "${rtk_tokens:-}" \
    '{
      name: $name,
      command: $command,
      budget: $budget,
      input_tokens: $input_tokens,
      axon_output_tokens: $output_tokens,
      axon_tokens_saved: $saved,
      axon_elapsed_ms: $elapsed_ms,
      ccr_recovered: $recovered,
      rtk_status: $rtk_status,
      rtk_output_tokens: (if $rtk_tokens == "" then null else ($rtk_tokens|tonumber) end)
    }' >> "$jsonl"
}

prepare_traces

run_case diff diff 500 "$diff_raw"
run_case grep grep 600 "$grep_raw"
run_case json json 600 "$json_raw"
run_case tsc tsc 500 "$tsc_raw"
run_case test test 700 "$test_raw"
run_case package package 300 "$package_raw"
run_case lint lint 180 "$lint_raw"
run_case log log 700 "$log_raw"

total_input="$(jq -s 'map(.input_tokens) | add' "$jsonl")"
total_axon="$(jq -s 'map(.axon_output_tokens) | add' "$jsonl")"
total_saved="$(jq -s 'map(.axon_tokens_saved) | add' "$jsonl")"
total_rtk="$(jq -s '[.[] | select(.rtk_output_tokens != null) | .rtk_output_tokens] | if length == 0 then null else add end' "$jsonl")"

{
  echo "# Shell Filter Aggregate Benchmark"
  echo
  echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "| Case | Budget | Input Tokens | Axon Tokens | Saved | CCR | RTK Status | RTK Tokens |"
  echo "| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |"
  jq -r '. | "| \(.name) | \(.budget) | \(.input_tokens) | \(.axon_output_tokens) | \(.axon_tokens_saved) | \(.ccr_recovered) | \(.rtk_status) | \(.rtk_output_tokens // "n/a") |"' "$jsonl"
  echo
  echo "Totals:"
  echo
  echo "- Input tokens: $total_input"
  echo "- Axon output tokens: $total_axon"
  echo "- Axon tokens saved: $total_saved"
  echo "- RTK output tokens for comparable cases: $total_rtk"
  echo
  echo "Artifacts:"
  echo
  echo "- JSONL: $jsonl"
  echo "- Per-case outputs: $out_dir/<case>/"
} > "$report"

cat "$report"
