# Completion Audit - Axon Context Optimization Objective - 2026-07-01

## Scope

Objective source: `/home/annonymous/.codex/attachments/93edcbc1-234b-443c-8423-17c06ba06d0d/pasted-text-1.txt`.

Audit method:

- Compare each explicit requirement against current code, tests, docs, and evidence files.
- Treat broad or indirect evidence as partial, not complete.
- Do not mark the persistent goal complete unless every "100%" requirement is directly proven.

Functional estimate: **99.7%** for implemented product behavior, **not complete** for the full absolute objective.

## Requirement Matrix

| Requirement | Status | Evidence | Residual Risk |
| --- | --- | --- | --- |
| Axon-first canonical context flows before raw reads | Proven for Axon and installed Claude Code projects | MCP and docs route context through `get_overview`, `get_context_capsule`, `get_skeleton`; capsule file entries now expose `source_ref` and Axon-first `expand_command`; installed `axon-shell-guard.sh` denies noisy raw Bash reads/searches in indexed projects; see `capsule-traceability-2026-07-01.md` and `shell-guard-hook-2026-07-01.md` | Non-Claude agents or manually modified Claude configs can still bypass Axon |
| Critical shell commands have native filters or compatible fallback | Proven for current critical set | Native filters: diff, grep/rg, JSON, tsc, test, package, lint, log; fallback `auto`/`text`; tests in `test_shell_filter`; evidence files per command family | Future command families need the same benchmark discipline |
| Large outputs classified before compression | Proven for implemented compression surfaces | `classify_output`, `OutputKind`, shell kind normalization; `compression-type-classification-2026-06-30.md`; `test_compress`, `test_shell_filter` | New compression entry points must call the same classifier |
| Compression is live-zone-only and does not mutate history, system prompt, tools, or cache hot zone | Proven for Axon-owned surfaces | Compression returns transformed output/capsule slices only; cache is bypassed for body-compression requests; shell filters transform stdin only | External agent prompt/history mutation is outside Axon's runtime control |
| Lossy compression has reversible recovery via artifact/retrieve | Proven | `ccr_artifacts`, `artifact_retrieve`, `/api/artifact`, `axon artifact-retrieve`; `ccr-artifact-retrieve-2026-06-30.md`; `shell-filter-ccr-2026-07-01.md`; `test_ccr` | Recovery depends on local DuckDB artifact retention |
| Compressors validate final output saves tokens | Proven | `compress_body`, shell filters, CCR marker-overhead fallback; `test_compress`, `test_shell_filter`, `test_ccr` | Token estimator is approximate `(bytes+3)/4`, by design |
| Error cases use safe passthrough without silent loss | Proven for current paths | Binary-like/impossible budget passthrough, malformed JSON/tsc/test/package/lint/log passthrough, DB-open fallback for shell CCR; tests cover malformed families | New filter families need explicit malformed-input tests |
| Savings metrics separated by layer | Proven | Telemetry layer column and aggregation for `retrieval`, `shell_filtering`, `compression`, `cache`, `ccr`, `unknown`; `telemetry-layers-2026-06-30.md`; `test_telemetry` | `indexing` is recorded but intentionally not part of savings layer docs |
| Native filters compared against RTK on real benchmarks | Proven for current native filters | Evidence files: diff, grep, JSON, tsc, test, package, lint, log; aggregate runner `scripts/benchmark_shell_filters.sh`; see `shell-filter-aggregate-benchmark-2026-07-01.md` | New filter families must be added to the runner |
| Capsules have explicit budget, traceable sources, and expansion on demand | Proven after this audit pass | `token_budget`, `source_ref`, `expand_command`, cache round-trip regression; `capsule-traceability-2026-07-01.md` | CLI capsule remains intentionally compact and does not print per-file metadata |
| Critical paths have fidelity, regression, and token-savings tests | Proven for current release surface | `ctest` covers parser, compression, shell filters, CCR, telemetry, dialogue, objectives, semantic, CLI JSON metrics, MCP capsule schema smoke (`mcp-capsule-schema-smoke-2026-07-01.md`), shell guard hook smoke (`shell-guard-hook-2026-07-01.md`), aggregate shell benchmark runner, docs freshness smoke, and CI/release gate wiring (`release-context-gate-2026-07-01.md`) | New critical paths must be added to the smoke/test matrix |
| Documentation reflects real behavior, not aspirational claims | Proven for known drift patterns | README/API/evidence updated; `axon init` template now emits documented config keys; `test_docs_freshness` rejects stale tool counts and obsolete shell-filter gaps; see `docs-freshness-smoke-2026-07-01.md` | Pattern list must evolve when new behavior claims are added |
| Axon can replace RTK as primary local context and shell-output optimization layer, keeping RTK optional | Proven for current release surface and installed Claude Code projects | Native shell filters cover current critical families; RTK evidence is comparative only; Axon CLI/MCP/HTTP expose primary local flows; migration guide added in `axon-primary-rtk-optional-2026-07-01.md`; Linux release workflow now runs the full CTest context gate before packaging (`release-context-gate-2026-07-01.md`); installed shell guard routes noisy Bash output to Axon first (`shell-guard-hook-2026-07-01.md`) | Non-Claude agents need equivalent policy/config to prevent bypass |

## Validations Run

```bash
cmake --build build --target axon test_objectives -j2
./build/tests/test_objectives --gtest_filter='*CapsuleFileTraceability*'
./build/axon capsule "shell filter metrics" --no-cache
./build/axon init "$(mktemp -d)"
rg -n "<legacy tool-count and shell-filter gap patterns>" README.md docs/en docs/evidence src/main.cpp
```

The final full build/test gate is recorded in the turn log:

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

Additional release-gate validation after CI/release workflow hardening:

```bash
bash -n scripts/benchmark_shell_filters.sh tests/smoke/test_shell_filter_aggregate_benchmark.sh tests/smoke/test_docs_freshness.sh tests/smoke/test_cli_filter_json_metrics.sh tests/smoke/test_mcp_capsule_schema.sh scripts/install.sh
find scripts -name "*.sh" -print0 | xargs -0 shellcheck
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Result: shell syntax passed, ShellCheck passed, full build passed, and CTest passed 13/13.

External-agent guard validation:

```bash
bash -n scripts/hooks/axon-shell-guard.sh tests/smoke/test_shell_guard_hook.sh scripts/install.sh
bash tests/smoke/test_shell_guard_hook.sh
```

Result: `shell_guard_hook_ok=true`.

## Next Gaps

1. Non-Claude agents remain outside Axon's installed hook boundary: they need equivalent MCP policy/config to prevent raw reads or unfiltered shell output.
2. Re-run the completion audit after branch review on a clean branch to decide whether the remaining external-governance risk is acceptable for calling the objective complete.

## Verdict

Do not mark the persistent objective complete yet. The implementation is release-gated for current Axon product surfaces and installed Claude Code projects, but the original objective asks for absolute 100% guarantees across canonical flows. The remaining gap is external-governance coverage for other agents or manually altered configs.
