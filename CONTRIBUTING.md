# Contributing to axon

Thank you for your interest in contributing. Axon welcomes bug fixes, new language parsers, documentation improvements, and performance work.

## Development Setup

```bash
git clone --recurse-submodules https://github.com/HideakiSolutions/axon.git
cd axon && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target axon -j 2
export LD_LIBRARY_PATH=$(pwd)/../third_party/duckdb/lib
```

> The build-guard hook caps CMake/Make/Ninja at `-j 2` while a Claude Code
> session is active (host runs MCP server + embeddings concurrently). Set
> `AXON_ALLOW_HIGH_PARALLELISM=1` for a one-off bump.

### Running the test suite

```bash
cmake -S . -B build -DAXON_BUILD_TESTS=ON
cmake --build build --target test_parser_smoke -j 2
ctest --test-dir build --output-on-failure
```

GoogleTest is pulled via FetchContent; a full network round-trip happens on
the first configure. The `AXON_BUILD_TESTS` option defaults ON when this
project is top-level and OFF when embedded.

Two smoke tests use optional dev tooling and **skip** (ctest reports
`***Skipped`) when it is absent, so a plain toolchain still gets a green
suite: `test_docs_freshness` needs `ripgrep`, and
`test_shell_filter_aggregate_benchmark` needs `ripgrep` + `jq`. CI installs
both; locally `apt/brew install ripgrep jq` runs them for real.

### CI / lint workflows

| Workflow | Trigger | Scope |
|----------|---------|-------|
| `.github/workflows/build.yml`   | push/PR to main+develop | matrix build (ubuntu-22.04 + macos-14) + ctest |
| `.github/workflows/lint.yml`    | push/PR to main+develop | shellcheck + clang-format-15 (advisory) |
| `.github/workflows/release.yml` | tag push (`v*.*.*`)     | multi-target tarball + GitHub Release |
| `.github/workflows/ci.yml`      | push/PR                 | shellcheck only — kept for compatibility |

## Adding a Language Parser

1. Add the tree-sitter grammar as a submodule:
   ```bash
   git submodule add https://github.com/tree-sitter/tree-sitter-<lang> third_party/tree-sitter-<lang>
   ```
2. Register in `src/parser/parser.cpp`: add extensions, grammar init, symbol extraction, import extraction
3. Add to `CMakeLists.txt`: include dir + source file
4. Add a fixture + assertions to `tests/unit/test_parser_smoke.cpp` covering the new symbol kinds — the test suite is the contract for what `parse_file()` is expected to emit
5. Run `ctest --test-dir build --output-on-failure` to validate
6. Smoke against a real project: `axon index /path/to/sample-<lang>-project && axon status`

## Adding a New MCP Tool

1. Add the tool definition object to `tools_list()` in `src/mcp/server.cpp`
2. Add a handler branch in `handle_tool()` in `src/mcp/server.cpp`
3. Implement logic in a new `src/core/` module if needed
4. Add the tool to `docs/en/api-reference.md`

## Commit Style

Conventional commits. Present tense, imperative mood:

```
feat(parser): add Kotlin import extraction
fix(db): use READ_ONLY mode for secondary repos
docs(readme): add multi-repo registry section
```

No AI attribution in commits (`Co-Authored-By: Claude` etc.).

## Pull Request Checklist

- [ ] Build passes (`make -j2`)
- [ ] `axon index` + `axon status` work on a real project
- [ ] Docs updated if adding/changing a tool or feature
- [ ] CHANGELOG entry added under `[Unreleased]`
- [ ] No breaking changes to existing MCP tool signatures without a migration note

## Code of Conduct

Be kind, be constructive, be inclusive.
