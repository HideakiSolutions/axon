# Telemetry Layers Evidence - 2026-06-30

## Scope

Change validated in this note:

- Telemetry events now carry an optimization `layer`.
- `/api/metrics`-style output keeps backward-compatible totals and adds `layers`.
- Layer keys are separated as `retrieval`, `shell_filtering`, `compression`, `cache`, `ccr`, and `unknown`.
- Legacy capsule events without an explicit layer infer `retrieval` or `cache`.
- Body-compressed capsules record a separate `compression` telemetry event when compression saves tokens.

This note does not claim complete shell-filter coverage. It proves metrics can now separate shell-filter savings when native shell events are recorded.

## Commands

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Results

Full CTest result:

```text
100% tests passed, 0 tests failed out of 6
```

Focused telemetry assertions in `tests/unit/test_telemetry.cpp` insert five events:

| Layer | Requests | Tokens Sent | Tokens Saved |
|-------|----------|-------------|--------------|
| retrieval | 1 | 100 | 300 |
| cache | 1 | 20 | 60 |
| compression | 1 | 50 | 150 |
| shell_filtering | 1 | 25 | 75 |
| ccr | 1 | 5 | 0 |

The aggregate output is also asserted:

| Metric | Value |
|--------|-------|
| requests | 5 |
| tokens_sent | 200 |
| tokens_saved | 585 |

## Fidelity Checks

- Existing total metric fields remain present for callers that do not read `layers`.
- Existing telemetry callers compile with inferred layers; explicit layer values were added to CLI indexing/cache/retrieval paths and MCP/HTTP request events.
- DuckDB migration adds `telemetry_events.layer` with default `unknown`, so existing databases keep loading.
- Remote telemetry payloads include `layer`.

## Remaining Gaps

- Native Axon shell filtering started later on 2026-06-30 with `axon filter`; see `docs/evidence/shell-filter-diff-2026-06-30.md`.
- CCR artifact/retrieve was added later on 2026-06-30; see `docs/evidence/ccr-artifact-retrieve-2026-06-30.md`.
