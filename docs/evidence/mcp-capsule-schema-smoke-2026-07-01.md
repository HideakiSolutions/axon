# MCP Capsule Schema Smoke Evidence - 2026-07-01

## Scope

- Exercise the real `axon serve` stdio JSON-RPC path.
- Call `tools/call` for `get_context_capsule`.
- Verify the actual MCP response includes capsule traceability and recovery fields:
  - `source_ref`
  - `expand_command`
  - `token_estimate`
  - `compression`
  - `ccr_artifact_ids`
  - `cache`

The smoke is skip-safe when Node.js or an embedding model is unavailable.

## Commands

```bash
cmake --build build -j2
bash tests/smoke/test_mcp_capsule_schema.sh ./build/axon
ctest --test-dir build --output-on-failure -R test_mcp_capsule_schema
```

## Result

```text
mcp_capsule_schema_ok=true
```

## Fidelity Checks

- The script creates an isolated temporary TypeScript project.
- It runs `axon init`, `axon index --force`, and then `axon serve` from that project root.
- The JSON-RPC request uses `tools/call` with `get_context_capsule`, explicit pivots, `token_budget=1000`, and `no_cache=true`.
- The verifier parses the JSON-RPC envelope and the nested MCP text payload.
- It fails if the capsule exceeds the requested budget, lacks compression counters, lacks `ccr_artifact_ids`, or omits per-file `source_ref`/`expand_command`.
