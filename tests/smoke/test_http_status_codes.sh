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
    wait "$serve_pid" 2>/dev/null || true
  fi
  # On Windows the serve's DuckDB handle can outlive the kill by a beat and
  # open files cannot be deleted; retry once, then best-effort (ephemeral
  # CI runner) — the assertions above already decided the test's outcome.
  rm -rf "$tmpdir" 2>/dev/null || { sleep 1; rm -rf "$tmpdir" 2>/dev/null || true; }
}
trap cleanup EXIT

# Hermetic registry + sandboxed project root.
export AXON_REGISTRY_DIR="$tmpdir/axon-home"
sandbox="$tmpdir/proj"
mkdir -p "$sandbox/.git" "$sandbox/src"
echo "ref: refs/heads/main" > "$sandbox/.git/HEAD"
cat > "$sandbox/src/auth.ts" <<'TS'
export function login(user: string): boolean {
  return user.length > 0;
}
TS
cat > "$sandbox/src/module.ts" <<'TS'
export const sharedValue = 42;
TS

(cd "$sandbox" && "$axon_bin" index . > /dev/null 2>&1)

# Two registered secondaries: one healthy graph and one deliberately stale
# registration. Both HTTP aggregation and MCP group_impact must expose typed
# failures, and neither is allowed to modify the healthy secondary database.
secondary="$tmpdir/secondary"
missing="$tmpdir/missing"
mkdir -p "$secondary/.git" "$secondary/src" "$missing/.git" "$missing/src"
echo "ref: refs/heads/main" > "$secondary/.git/HEAD"
echo "ref: refs/heads/main" > "$missing/.git/HEAD"
cat > "$secondary/src/module.ts" <<'TS'
export const sharedValue = 42;
TS
cat > "$secondary/src/consumer.ts" <<'TS'
import { sharedValue } from './module';
export const consumed = sharedValue;
TS
echo 'export const stale = true;' > "$missing/src/stale.ts"
(cd "$secondary" && "$axon_bin" index . > /dev/null 2>&1)
(cd "$missing" && "$axon_bin" index . > /dev/null 2>&1)
rm "$missing/.axon/index.duckdb"
if command -v sha256sum >/dev/null 2>&1; then
  secondary_hash_before="$(sha256sum "$secondary/.axon/index.duckdb" | awk '{print $1}')"
else
  secondary_hash_before="$(shasum -a 256 "$secondary/.axon/index.duckdb" | awk '{print $1}')"
fi

port=$((20000 + RANDOM % 20000))
# Launch the serve as a direct child (no subshell wrapper): $! must be the
# real axon pid so cleanup's kill reaches the process that holds the DuckDB
# lock — under MSYS, killing a wrapper subshell leaves the native exe alive.
cd "$sandbox"
"$axon_bin" serve --http --port="$port" --all > "$tmpdir/serve.log" 2>&1 &
serve_pid=$!
# Step back out: cleanup removes $tmpdir, and Windows cannot delete the
# current directory of a live process.
cd /

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

graph_response="$(curl -sf "http://127.0.0.1:${port}/api/graph")"
if grep -q 'secondary_errors' <<<"$graph_response" &&
   grep -q 'database_unavailable' <<<"$graph_response" &&
   grep -q 'secondary/src/consumer.ts\|secondary/consumer.ts' <<<"$graph_response"; then
  echo "PASS: /api/graph aggregates read-only and reports typed secondary failures"
else
  echo "FAIL: /api/graph missing aggregated graph or typed secondary failure"
  fail=1
fi

kill "$serve_pid" 2>/dev/null || true
wait "$serve_pid" 2>/dev/null || true
serve_pid=""

mcp_request='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"group_impact","arguments":{"file":"src/module.ts"}}}'
mcp_response="$(cd "$sandbox" && printf '%s\n' "$mcp_request" | "$axon_bin" serve 2>"$tmpdir/mcp.log")"
if grep -q 'consumer.ts' <<<"$mcp_response" &&
   grep -q 'database_unavailable' <<<"$mcp_response" &&
   grep -q 'failures' <<<"$mcp_response"; then
  echo "PASS: group_impact uses current edge columns and reports typed failures"
else
  echo "FAIL: group_impact result was incomplete"
  cat "$tmpdir/mcp.log"
  fail=1
fi

if command -v sha256sum >/dev/null 2>&1; then
  secondary_hash_after="$(sha256sum "$secondary/.axon/index.duckdb" | awk '{print $1}')"
else
  secondary_hash_after="$(shasum -a 256 "$secondary/.axon/index.duckdb" | awk '{print $1}')"
fi
check "secondary database remains byte-identical" "$secondary_hash_before" "$secondary_hash_after"

# A syntactically valid registry with a wrong field type must not abort or be
# overwritten during serve startup. The tool reports the load error fail-closed.
group_list_request='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"group_list","arguments":{}}}'
for malformed_registry in \
  '{"schema_version":2,"repos":[],"groups":{},"storage_profiles":{}}' \
  '{"schema_version":"axon-registry/v2","repos":{},"groups":{},"storage_profiles":{}}'; do
  printf '%s' "$malformed_registry" > "$AXON_REGISTRY_DIR/registry.json"
  set +e
  malformed_response="$(cd "$sandbox" && printf '%s\n' "$group_list_request" | "$axon_bin" serve 2>"$tmpdir/malformed.log")"
  malformed_status=$?
  set -e
  registry_after="$(<"$AXON_REGISTRY_DIR/registry.json")"
  if [[ "$malformed_status" == "0" ]] &&
     grep -q 'invalid_registry_type' <<<"$malformed_response" &&
     [[ "$registry_after" == "$malformed_registry" ]]; then
    echo "PASS: malformed registry type fails closed without abort or overwrite"
  else
    echo "FAIL: malformed registry handling status=$malformed_status response=$malformed_response"
    cat "$tmpdir/malformed.log"
    fail=1
  fi
done

exit $fail
