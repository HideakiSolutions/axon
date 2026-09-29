#!/usr/bin/env bash
# Smoke: `axon filter --metrics=json` + CCR recovery, in three scenarios:
#   1. unlocked index      → artifact stored in DuckDB, retrieved from DuckDB
#   2. index locked        → artifact stored in the file sidecar (.axon/ccr/),
#      (another serve)       retrieved from the sidecar
#   3. DB-stored artifact  → retrieved under lock via the #58 peer proxy
#      retrieved under lock  (the lock-holding serve answers /rpc/tool)
#
# The whole test runs inside a hermetic sandbox: a sentinel .git/HEAD stops
# find_project_root from walking up into this repo (whose index may be locked
# by an unrelated serve), and HOME is redirected so the multi-repo registry
# used by the peer lookup never touches the real ~/.axon.
set -euo pipefail

axon_bin="${1:?axon binary path required}"
# Absolute paths include drive-letter forms (D:/... or D:\...) that ctest
# passes on Windows runners — only prefix genuinely relative paths.
case "$axon_bin" in /* | [A-Za-z]:[/\\]*) ;; *) axon_bin="$(pwd)/$axon_bin" ;; esac

trap 'echo "[test_cli_filter_json_metrics] FAILED at line $LINENO" >&2' ERR

tmpdir="$(mktemp -d)"
serve_pid=""
cleanup() {
  [ -n "$serve_pid" ] && kill "$serve_pid" 2>/dev/null || true
  exec 9>&- 2>/dev/null || true
  rm -rf "$tmpdir"
}
trap cleanup EXIT

export HOME="$tmpdir"
sandbox="$tmpdir/sandbox"
mkdir -p "$sandbox/.git"
echo "ref: refs/heads/main" > "$sandbox/.git/HEAD"
cd "$sandbox"

make_raw() {
  local out="$1" tag="$2"
  : > "$out"
  for file in $(seq 0 7); do
    for line in $(seq 1 12); do
      printf 'src/module%s.cpp:%s: matched %s metric payload payload payload payload payload\n' \
        "$file" "$line" "$tag" >> "$out"
    done
  done
}

# ── Section 1: unlocked index — DuckDB store ───────────────────────────────
raw="$tmpdir/raw.txt"
make_raw "$raw" "filter"

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

# ── Hold the index lock with a stdio serve (peer listener active) ──────────
# The locked-index sections drive a long-lived serve whose stdin is a FIFO
# held open on fd 9. MSYS/Git Bash emulates FIFOs over named pipes and the
# native-exe-reading-from-FIFO arrangement is not reliable there, so on
# Windows we stop after section 1 (unlocked DuckDB store + retrieve, already
# asserted above) and leave lock/sidecar/peer-proxy coverage to Linux/macOS.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "[test_cli_filter_json_metrics] SKIP locked-index sections on Windows:" \
         "FIFO-backed serve stdin is unreliable under Git Bash; sections 2-3" \
         "covered on Linux/macOS"
    exit 0
    ;;
esac

fifo="$tmpdir/serve-stdin.fifo"
mkfifo "$fifo"
"$axon_bin" serve < "$fifo" > "$tmpdir/serve.log" 2>&1 &
serve_pid=$!
exec 9> "$fifo"   # keep the serve's stdin open so it stays alive

# Nudge the serve with one tool call: if its startup DB open lost a race
# (any CLI command opens the same DuckDB briefly), ensure_db_open re-takes
# the lock and registers ownership on this first call. Do NOT poll with
# `axon status` here — that opens the DB and is exactly the race that can
# push the serve into a db-less startup.
printf '%s\n' '{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"get_overview","arguments":{}}}' >&9

# Readiness = owner registration in the sandbox registry (implies the serve
# holds the DB lock); section 3's peer-proxy lookup depends on it too.
owner_ready=0
for _ in $(seq 1 100); do
  if grep -q '"owner_port"' "$HOME/.axon/registry.json" 2>/dev/null; then
    owner_ready=1
    break
  fi
  sleep 0.1
done
if [ "$owner_ready" != 1 ]; then
  echo "serve never registered itself as repo owner; log:" >&2
  cat "$tmpdir/serve.log" >&2
  exit 1
fi

# Sanity: with the serve owning the lock, a direct CLI open must fail.
if "$axon_bin" status >/dev/null 2>&1; then
  echo "expected the serve to hold the index lock, but status succeeded" >&2
  exit 1
fi

# ── Section 2: locked index — file sidecar store + retrieve ───────────────
raw2="$tmpdir/raw-locked.txt"
make_raw "$raw2" "locked"

"$axon_bin" filter grep --budget=260 --metrics=json < "$raw2" > "$tmpdir/filtered2.txt" 2> "$tmpdir/metrics2.json"
grep -q '"changed":true' "$tmpdir/metrics2.json"
grep -q '"recoverable":true' "$tmpdir/metrics2.json"
grep -Eq '"tokens_saved":[1-9][0-9]*' "$tmpdir/metrics2.json"

artifact_id2="$(sed -n 's/.*"ccr_artifact_id":"\([^"]*\)".*/\1/p' "$tmpdir/metrics2.json")"
[ -f ".axon/ccr/${artifact_id2}.json" ] || { echo "sidecar artifact file missing" >&2; exit 1; }
"$axon_bin" artifact-retrieve "$artifact_id2" > "$tmpdir/recovered2.txt"
cmp -s "$raw2" "$tmpdir/recovered2.txt"

# ── Section 3: DB-stored artifact retrieved under lock via peer proxy ─────
"$axon_bin" artifact-retrieve "$artifact_id" > "$tmpdir/recovered3.txt"
cmp -s "$raw" "$tmpdir/recovered3.txt"

kill "$serve_pid" 2>/dev/null || true
wait "$serve_pid" 2>/dev/null || true
serve_pid=""
