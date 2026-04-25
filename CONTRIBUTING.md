# Contributing to axon

Thank you for your interest in contributing. Axon welcomes bug fixes, new language parsers, documentation improvements, and performance work.

## Development Setup

```bash
git clone --recurse-submodules https://github.com/HideakiSolutions/axon.git
cd axon && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j2
export LD_LIBRARY_PATH=$(pwd)/../third_party/duckdb/lib
```

## Adding a Language Parser

1. Add the tree-sitter grammar as a submodule:
   ```bash
   git submodule add https://github.com/tree-sitter/tree-sitter-<lang> third_party/tree-sitter-<lang>
   ```
2. Register in `src/parser/parser.cpp`: add extensions, grammar init, symbol extraction, import extraction
3. Add to `CMakeLists.txt`: include dir + source file
4. Test: `axon index /path/to/sample-<lang>-project && axon status`

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
