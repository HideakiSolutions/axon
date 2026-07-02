#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

tool_count="$(rg -n '\{\{"name","' src/mcp/server.cpp | wc -l | tr -d ' ')"
if [[ "$tool_count" != "27" ]]; then
  echo "expected 27 MCP tools in src/mcp/server.cpp, found $tool_count" >&2
  exit 1
fi

grep -q 'MCP Tools (27)' README.md
grep -q 'All 27 MCP tools' docs/en/api-reference.md
grep -q 'all 27 MCP tools' docs/en/getting-started.md
grep -q 'No. All 27 MCP tools work without it.' docs/en/faq.md
grep -q '25 other tools' README.md
grep -q 'raw shell-output' README.md
grep -q 'axon-shell-guard.sh' scripts/templates/CLAUDE.md
grep -q 'axon-shell-guard.sh' docs/en/axon-primary-rtk-optional.md
grep -q 'axon-shell-guard.sh' docs/pt-br/axon-primary-rtk-optional.md
grep -q 'test_shell_guard_hook' tests/CMakeLists.txt

stale_patterns=(
  '24 other tools'
  '26 MCP'
  'MCP-26'
  '26 tool handlers'
  'file-granular (not symbol-granular) in the current release'
  'human-readable metrics to stderr; a machine-readable mode may be useful'
  'filter currently emits human-readable metrics'
  'richer log traces'
  'No single benchmark runner aggregates all scenarios yet'
  'End-to-end MCP JSON-RPC response schema is build-covered but not deeply smoke-tested'
)

for pattern in "${stale_patterns[@]}"; do
  if rg -n "$pattern" README.md docs/en docs/evidence src/main.cpp >/tmp/axon-docs-freshness-hit.txt; then
    cat /tmp/axon-docs-freshness-hit.txt >&2
    echo "stale documentation pattern found: $pattern" >&2
    exit 1
  fi
done

grep -q 'shell-filter-aggregate-benchmark-2026-07-01.md' docs/evidence/completion-audit-2026-07-01.md
grep -q 'mcp-capsule-schema-smoke-2026-07-01.md' docs/evidence/completion-audit-2026-07-01.md
grep -q 'docs-freshness-smoke-2026-07-01.md' docs/evidence/completion-audit-2026-07-01.md
grep -q 'shell-guard-hook-2026-07-01.md' docs/evidence/completion-audit-2026-07-01.md
grep -q 'axon-primary-rtk-optional.md' docs/en/getting-started.md
grep -q 'axon-primary-rtk-optional.md' docs/pt-br/getting-started.md

echo "docs_freshness_ok=true"
