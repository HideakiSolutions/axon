#!/usr/bin/env bash
# tests/e2e/smoke.sh — end-to-end smoke for axon, exercising the user
# surface that unit tests don't reach: examples/ indexing, capsule cache
# round-trip, .axonignore glob behavior, and CLI exit codes.
#
# Run from the repo root after building:
#   cmake --build build --target axon -j 2
#   tests/e2e/smoke.sh
#
# Each section is self-contained and prints PASS/FAIL with a one-line reason.
# The script exits non-zero on the first failure (set -e).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AXON="${AXON:-${REPO_ROOT}/build/axon}"
TMP_BASE="${TMPDIR:-/tmp}/axon-e2e-$$"
mkdir -p "${TMP_BASE}"

# Tracking
PASS_COUNT=0
FAIL_COUNT=0

cleanup() {
  rm -rf "${TMP_BASE}" 2>/dev/null || true
}
trap cleanup EXIT

log() { printf '[smoke] %s\n' "$*"; }
pass() { printf '[smoke] \e[32mPASS\e[0m %s\n' "$1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { printf '[smoke] \e[31mFAIL\e[0m %s\n' "$1"; FAIL_COUNT=$((FAIL_COUNT + 1)); return 1; }

assert_contains() {
  local needle="$1" haystack="$2" desc="$3"
  if printf '%s' "$haystack" | grep -q "$needle"; then
    pass "$desc"
  else
    fail "$desc — expected '$needle' in:\n${haystack}"
  fi
}

assert_eq() {
  local got="$1" want="$2" desc="$3"
  if [ "$got" = "$want" ]; then
    pass "$desc"
  else
    fail "$desc — got '$got', want '$want'"
  fi
}

# ── Sanity ─────────────────────────────────────────────────────────────────
log "axon binary: $AXON"
[ -x "$AXON" ] || { echo "axon binary missing: $AXON" >&2; exit 1; }

VERSION_OUTPUT=$("$AXON" --version)
assert_contains "axon 1\." "$VERSION_OUTPUT" "--version prints a 1.x line"

HELP_OUTPUT=$("$AXON" help 2>&1)
assert_contains "axon capsule" "$HELP_OUTPUT" "help mentions capsule subcommand"
assert_contains "axon web" "$HELP_OUTPUT" "help mentions web subcommand"
assert_contains "axon lsp" "$HELP_OUTPUT" "help mentions lsp subcommand"

LSP_INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
LSP_OUTPUT=$(printf 'Content-Length: %s\r\n\r\n%s' "${#LSP_INIT}" "$LSP_INIT" | "$AXON" lsp)
assert_contains "axon-lsp" "$LSP_OUTPUT" "lsp initialize returns axon-lsp serverInfo"

# ── Examples: ts-mini ──────────────────────────────────────────────────────
# axon walks up looking for project markers (.git, Cargo.toml, etc.) — drop
# a sentinel .git/HEAD so find_project_root stops at our test dir instead
# of leaking into /tmp's parent. Each test dir gets the same treatment.
TS_DIR="${TMP_BASE}/ts-mini"
cp -r "${REPO_ROOT}/examples/ts-mini" "$TS_DIR"
mkdir -p "$TS_DIR/.git" && echo "ref: refs/heads/main" > "$TS_DIR/.git/HEAD"
( cd "$TS_DIR" && "$AXON" init >/dev/null )
( cd "$TS_DIR" && "$AXON" index 2>&1 | tail -3 )

TS_STATUS=$( cd "$TS_DIR" && "$AXON" status )
assert_contains "Files:" "$TS_STATUS" "ts-mini status reports Files line"
TS_FILES=$(printf '%s' "$TS_STATUS" | awk '/^Files:/ {print $2}')
[ "$TS_FILES" -ge 4 ] && pass "ts-mini indexed >= 4 files (got $TS_FILES)" || fail "ts-mini indexed too few files: $TS_FILES"

if command -v curl >/dev/null 2>&1; then
  WEB_PORT=$((17070 + ($$ % 1000)))
  WEB_LOG="${TMP_BASE}/axon-web.log"
  ( cd "$TS_DIR" && "$AXON" web --port="${WEB_PORT}" >"$WEB_LOG" 2>&1 ) &
  WEB_PID=$!
  sleep 1
  WEB_HTML=$(curl -fsS "http://127.0.0.1:${WEB_PORT}/" || true)
  kill "$WEB_PID" >/dev/null 2>&1 || true
  wait "$WEB_PID" >/dev/null 2>&1 || true
  assert_contains "Axon Web" "$WEB_HTML" "web serves browser UI at /"
else
  log "curl unavailable — skipping web HTTP smoke"
fi

# ── Capsule cache: miss → hit → no-cache ──────────────────────────────────
# `axon capsule` needs an embedding model on the miss path. In CI we may
# not have one staged, so detect absence (via a dry-run that is allowed
# to fail) and skip the cache section gracefully — the structural tests
# above still validate the binary.
HAS_MODEL=1
if ! ( cd "$TS_DIR" && "$AXON" capsule "_probe_" >/dev/null 2>&1 ); then
  if [ "${AXON_REQUIRE_MODEL:-0}" = "1" ]; then
    fail "capsule probe failed and AXON_REQUIRE_MODEL=1 is set"
  else
    log "embedding model unavailable — skipping capsule cache section"
    HAS_MODEL=0
  fi
fi

if [ "$HAS_MODEL" = 1 ]; then
  # Re-run from a clean state so the probe above doesn't pre-warm the cache.
  ( cd "$TS_DIR" && rm -f .axon/index.duckdb && "$AXON" index >/dev/null 2>&1 || true )

  CAPSULE_1=$( cd "$TS_DIR" && "$AXON" capsule "user authentication" 2>/dev/null )
  if printf '%s' "$CAPSULE_1" | grep -q '"cache": "hit"'; then
    fail "first capsule call reported cache hit (expected miss)"
  else
    pass "first capsule call is a miss (no cache field)"
  fi

  CAPSULE_2=$( cd "$TS_DIR" && "$AXON" capsule "user authentication" 2>/dev/null )
  assert_contains '"cache": "hit"' "$CAPSULE_2" "second capsule call is a cache hit"

  CAPSULE_3=$( cd "$TS_DIR" && "$AXON" capsule "user authentication" --no-cache 2>/dev/null )
  if printf '%s' "$CAPSULE_3" | grep -q '"cache": "hit"'; then
    fail "--no-cache still reported cache hit"
  else
    pass "--no-cache forces miss path"
  fi
fi

# ── .axonignore globbing ──────────────────────────────────────────────────
GLOB_DIR="${TMP_BASE}/globtest"
mkdir -p "$GLOB_DIR/generated" "$GLOB_DIR/src" "$GLOB_DIR/.git"
echo "ref: refs/heads/main" > "$GLOB_DIR/.git/HEAD"
cat > "$GLOB_DIR/.axonignore" <<'IGN'
*.log
**/generated/**
IGN
echo 'def kept(): pass'    > "$GLOB_DIR/src/kept.py"
echo 'def ignored(): pass' > "$GLOB_DIR/generated/ignored.py"
echo 'noise'               > "$GLOB_DIR/foo.log"
( cd "$GLOB_DIR" && "$AXON" init >/dev/null )
( cd "$GLOB_DIR" && "$AXON" index >/dev/null 2>&1 || true )
GLOB_STATUS=$( cd "$GLOB_DIR" && "$AXON" status )
GLOB_FILES=$(printf '%s' "$GLOB_STATUS" | awk '/^Files:/ {print $2}')
# Only kept.py should be indexed (.log files and generated/ excluded; keep.log
# is a .log file — even with !keep.log negation it's still excluded because
# .log isn't a recognized extension for parsing in the first place).
# Only src/kept.py should be indexed: foo.log matches *.log, generated/ignored.py
# matches **/generated/**.
[ "$GLOB_FILES" = "1" ] && pass ".axonignore glob excludes generated/ and *.log (1 file indexed)" \
                       || fail ".axonignore glob mis-indexed; expected 1 got $GLOB_FILES"

# ── Skeleton on a real example file ───────────────────────────────────────
SKEL_OUT=$( "$AXON" skeleton "${REPO_ROOT}/examples/python-mini/app/routes.py" 2>/dev/null )
assert_contains "list_users" "$SKEL_OUT" "skeleton extracts list_users from python-mini"

# ── Wrap-up ────────────────────────────────────────────────────────────────
log ""
log "Summary: ${PASS_COUNT} pass, ${FAIL_COUNT} fail"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
exit 0
