# Docs Freshness Smoke Evidence - 2026-07-01

## Scope

- Add a lightweight docs/evidence freshness gate for stale claims that have previously drifted during this convergence loop.
- Verify MCP tool count references remain aligned with the source tool list.
- Fail on stale shell-filter gap language after JSON metrics, log filtering, MCP capsule schema smoke, and aggregate benchmarks have landed.

## Command

```bash
bash tests/smoke/test_docs_freshness.sh
ctest --test-dir build --output-on-failure -R test_docs_freshness
```

## Result

```text
docs_freshness_ok=true
```

## Fidelity Checks

- Counts `{{"name","...` tool declarations in `src/mcp/server.cpp` and expects 27.
- Verifies README/API/getting-started/FAQ refer to 27 MCP tools.
- Verifies README's short intro says `25 other tools`.
- Rejects stale phrases for outdated tool counts, obsolete log-filter gaps, and obsolete machine-readable metrics gaps.
- Verifies completion audit references the aggregate shell-filter benchmark evidence.
- Verifies getting-started docs link to the Axon-primary / RTK-optional guide.
