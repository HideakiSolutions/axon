# Release Context Optimization Gate - 2026-07-01

## Scope

Close the audit gap that required aggregate shell-filter benchmarks and docs freshness checks to run in repeatable CI/release paths, not only as local evidence.

## Changes

- `.github/workflows/build.yml`
  - Installs `ripgrep` alongside `jq` so docs freshness and benchmark smoke tests have their runtime dependencies.
  - Builds all configured targets with `cmake --build build -j 2` before `ctest`, ensuring newly added test binaries are present.
- `.github/workflows/release.yml`
  - Installs `jq` and `ripgrep` for the Linux release job.
  - Builds all test targets on Linux release runs.
  - Runs `ctest --test-dir build --output-on-failure` before packaging, which includes:
    - shell filter JSON metrics smoke
    - MCP capsule schema smoke
    - docs freshness smoke
    - aggregate shell-filter benchmark gate
- `.github/workflows/lint.yml`
  - ShellCheck now covers every `scripts/*.sh` file, including `scripts/benchmark_shell_filters.sh`.

## Local Validation

```bash
bash -n scripts/benchmark_shell_filters.sh \
  tests/smoke/test_shell_filter_aggregate_benchmark.sh \
  tests/smoke/test_docs_freshness.sh \
  tests/smoke/test_cli_filter_json_metrics.sh \
  tests/smoke/test_mcp_capsule_schema.sh \
  scripts/install.sh

cmake --build build -j2
ctest --test-dir build --output-on-failure

find scripts -name "*.sh" -print0 | xargs -0 shellcheck
```

Result:

- Shell syntax validation passed.
- ShellCheck passed for every script under `scripts/`.
- Full build passed.
- `ctest` passed 13/13 tests, including `test_docs_freshness`, `test_shell_guard_hook`, and `test_shell_filter_aggregate_benchmark`.
