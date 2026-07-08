#!/usr/bin/env bash
# Smoke: HTTP API error semantics. /api/capsule used to answer HTTP 200 with
# {"error": ...} for every failure (missing q, DB not ready, model missing),
# so clients could not tell success from failure without parsing the body.
# Asserts: 400 (missing q), 404 (unknown artifact), and 200-or-503 for a
# well-formed capsule request (503 when no embedding model is staged — CI).
set -euo pipefail

axon_bin="$(realpath "${1:?axon binary path required}")"

tmpdir="$(mktemp -d)"
serve_pid=""
# shellcheck disable=SC2317  # invoked indirectly via trap
cleanup() {
  if [[ -n "$serve_pid" ]]; then
    kill "$serve_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT

# Hermetic registry + sandboxed project root.
export AXON_HOME="$tmpdir/axon-home"
sandbox="$tmpdir/proj"
mkdir -p "$sandbox/.git" "$sandbox/src"
echo "ref: refs/heads/main" > "$sandbox/.git/HEAD"
cat > "$sandbox/src/auth.ts" <<'TS'
export function login(user: string): boolean {
  return user.length > 0;
}
TS

(cd "$sandbox" && "$axon_bin" index . > /dev/null 2>&1)

port=$((20000 + RANDOM % 20000))
(cd "$sandbox" && "$axon_bin" serve --http --port="$port" > "$tmpdir/serve.log" 2>&1) &
serve_pid=$!

for _ in $(seq 1 60); do
  curl -sf "http://127.0.0.1:${port}/api/overview" -o /dev/null 2>/dev/null && break
  kill -0 "$serve_pid" 2>/dev/null || { echo "FAIL: serve died"; cat "$tmpdir/serve.log"; exit 1; }
  sleep 0.5
done

fail=0
check() { # desc expected actual
  if [[ "$3" == "$2" ]]; then
    echo "PASS: $1 -> $3"
  else
    echo "FAIL: $1 -> got $3, want $2"
    fail=1
  fi
}

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${port}/api/capsule")
check "/api/capsule without q is 400" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${port}/api/artifact/ccr_does_not_exist")
check "/api/artifact unknown id is 404" 404 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${port}/api/capsule?q=login&budget=2000")
if [[ "$code" == "200" || "$code" == "503" ]]; then
  echo "PASS: /api/capsule with q is 200 (model staged) or 503 (no model) -> $code"
else
  echo "FAIL: /api/capsule with q -> got $code, want 200 or 503"
  fail=1
fi

exit $fail
