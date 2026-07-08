#!/usr/bin/env bash
set -euo pipefail

axon_bin="$(realpath "${1:?axon binary path required}")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node is required to parse nested MCP JSON"
  exit 0
fi

binary_dir="$(cd "$(dirname "$axon_bin")" && pwd)"
model_name="nomic-embed-text-v1.5.Q4_K_M.gguf"
if [[ ! -f "$binary_dir/../models/$model_name" &&
      ! -f "$binary_dir/../../models/$model_name" &&
      ! -f "${HOME:-}/.axon/models/$model_name" &&
      -z "${AXON_EMBEDDING_MODEL:-}" ]]; then
  echo "SKIP: embedding model not available for MCP capsule smoke"
  exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Hermetic registry: `axon index` below must not pollute ~/.axon/registry.json.
export AXON_HOME="$tmpdir/axon-home"

mkdir -p "$tmpdir/src/auth"
cat > "$tmpdir/package.json" <<'JSON'
{"name":"axon-mcp-capsule-smoke","private":true}
JSON
cat > "$tmpdir/src/auth/token.ts" <<'TS'
export function issueToken(userId: string): string {
  const ttlSeconds = 60 * 60;
  return `${userId}:${ttlSeconds}`;
}
TS
cat > "$tmpdir/src/auth/session.ts" <<'TS'
import { issueToken } from "./token";

export function createSession(userId: string): string {
  return issueToken(userId);
}
TS

"$axon_bin" init "$tmpdir" >/dev/null
"$axon_bin" index "$tmpdir" --force >/dev/null 2> "$tmpdir/index.err"

request='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_context_capsule","arguments":{"query":"token ttl","pivot_files":["src/auth/token.ts"],"token_budget":1000,"no_cache":true}}}'
(
  cd "$tmpdir"
  printf 'Content-Length: %s\r\n\r\n%s' "${#request}" "$request" | "$axon_bin" serve
) > "$tmpdir/response.txt" 2> "$tmpdir/server.err"

node "$script_dir/verify_mcp_capsule_schema.js" "$tmpdir/response.txt"
