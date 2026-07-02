# Capsule Traceability Evidence - 2026-07-01

## Scope

- Capsule file entries now carry `source_ref` and `expand_command`.
- `source_ref` points to the originating file or line span when the capsule was assembled from symbol slices.
- `expand_command` points back to Axon-first expansion paths instead of raw file reads:
  - `get_skeleton {"files":[...]}`
  - `get_context_capsule {"pivot_files":[...],"no_cache":true}`
- Capsule cache serialization preserves traceability metadata so cache hits and fresh assembly expose the same fields.

## Commands

```bash
cmake --build build --target axon test_objectives -j2
./build/tests/test_objectives --gtest_filter='*CapsuleFileTraceability*'
./build/axon capsule "shell filter metrics" --no-cache > /tmp/axon-capsule-trace.json 2> /tmp/axon-capsule-trace.err
```

## Results

Targeted regression:

```text
[ RUN      ] ObjTest.CapsuleFileTraceabilitySurvivesCacheRoundTrip
[       OK ] ObjTest.CapsuleFileTraceabilitySurvivesCacheRoundTrip
```

Real capsule smoke with the local embedding model:

```json
{
  "query": "shell filter metrics",
  "token_estimate": 3195,
  "pivot_files": 5,
  "support_files": 17,
  "compression_tokens_saved": 0
}
```

The CLI smoke confirms capsule assembly still works. The metadata is exposed on structured MCP and HTTP capsule file entries; the CLI intentionally prints a compact count summary plus file list on stderr.

## Fidelity Checks

- Unit test verifies `source_ref` and `expand_command` survive a cache insert/lookup round trip.
- Build covers MCP and HTTP response renderers that include the new fields.
- The expansion hints route agents back through Axon tools, preserving the objective's Axon-first retrieval path.
