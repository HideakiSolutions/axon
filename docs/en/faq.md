# FAQ — axon

## Getting Started

**Q: Do I need the embedding model to use axon?**
No. All 27 MCP tools work without it. The embedding model only enables `search_memory`, `turn_search`, `dialogue_context` and the semantic-query path of `get_context_capsule`. Without it, `get_context_capsule` falls back to graph-based pivot selection.

**Q: Can I use axon with editors other than Claude Code?**
Yes. Axon speaks stdio JSON-RPC 2.0 (the MCP protocol). Any MCP-compatible client works. HTTP mode also exposes a plain REST API.

**Q: How long does indexing take?**
A 1,000-file TypeScript project takes ~15–30s on first index. Subsequent incremental reindexes (write-through hooks) are near-instant.

**Q: Does axon send my code anywhere?**
No. Everything runs locally. The embedding model runs via llama.cpp on your CPU. DuckDB is embedded. No network calls are made.

---

## Usage

**Q: What's the difference between `get_context_capsule` and `get_skeleton`?**
`get_skeleton` returns signatures only for the files you specify. `get_context_capsule` selects the pivot files automatically (via query or graph centrality), returns them in full, and returns their neighbors as skeletons — all within a token budget.

**Q: When should I use `get_overview` vs `get_context_capsule`?**
Use `get_overview` at the start of a session on an unfamiliar codebase — it shows the most connected files and most-referenced symbols, like a map. Use `get_context_capsule` when you have a specific query or task.

**Q: Why does `get_callers` return files, not call sites?**
By default, Axon's dependency edges are file-granular. When `granularity = "symbol"` is enabled and the project is reindexed, `get_callers` also returns `caller_symbols`. To narrow broad file-level results, follow up with `get_skeleton(caller_files)` or expand from a capsule file's `expand_command`.

**Q: How does `detect_changes` work?**
It runs `git diff --unified=0` from the specified ref, parses hunk positions, and finds symbols whose line ranges overlap with the diff. Then it runs `get_impact_graph` on the changed files to return downstream impact.

**Q: Can I use axon on a monorepo with multiple languages?**
Yes. Axon handles mixed-language projects natively. The dependency graph crosses language boundaries for files that import each other.

**Q: What does `rename` actually do with `dry_run=false`?**
It finds all files containing the symbol name (cross-referenced with the graph), applies the rename at each location, and writes the files to disk. Always use `dry_run=true` first to review the proposed changes.

**Q: How does `group_impact` work?**
It looks up all repos in `~/.axon/registry.json`, opens each secondary DB in read-only mode, and searches for files whose import paths match the module path of the specified file. Useful for detecting cross-repo breaking changes.

---

## Configuration

**Q: Where does axon store its index?**
In `.axon/index.duckdb` at the project root (detected via `.git` walk-up).

**Q: How do I change the token budget for capsules?**
Pass `token_budget` to `get_context_capsule`. Default is 8000 tokens.

**Q: Can I configure which file extensions axon indexes?**
Not yet. Axon indexes the built-in supported extensions and uses `.axonignore` for path-level exclusions. `.axon/config.toml` currently supports `granularity`, `index_routes`, `fts_enabled`, `token_budget`, `telemetry`, and `capsule_compression`.

**Q: How do I add a project to a named group?**
Edit `~/.axon/registry.json` directly, or use `axon serve --group=<name>` — axon will auto-register the current repo and associate it with the group.

---

## Troubleshooting

**Q: `axon: error while loading shared libraries: libduckdb.so`**
Release packages are relocatable and should not need `LD_LIBRARY_PATH`. For source-tree runs, set it manually:
```bash
export LD_LIBRARY_PATH=/path/to/axon/third_party/duckdb/lib
```

**Q: Build fails with `fatal error: llama.h: No such file or directory`**
Submodules were not initialized:
```bash
git submodule update --init --recursive
```

**Q: `search_memory` returns nothing**
The embedding model is not loaded. Check that `models/nomic-embed-text-v1.5.Q4_K_M.gguf` exists and `AXON_EMBEDDING_MODEL` points to it when using a custom path.

**Q: Claude Code shows axon as disconnected**
1. If running from a source tree, check `LD_LIBRARY_PATH` is set in the MCP `env` block in `~/.claude.json`
2. Run `axon serve` manually and check for errors
3. Verify the binary path is correct

---

## Contributing

**Q: How do I add a new language parser?**
1. Add the tree-sitter grammar as a submodule in `third_party/`
2. Register the language in `src/parser/parser.cpp` — add extensions + grammar init + symbol/import extraction logic
3. Add the grammar to `CMakeLists.txt`

**Q: How do I add a new MCP tool?**
1. Add the tool definition to `tools_list()` in `src/mcp/server.cpp`
2. Add a handler branch in `handle_tool()` in `src/mcp/server.cpp`
3. Implement the logic in a new `src/core/` module if needed

**Q: Where do I report bugs?**
[GitHub Issues](https://github.com/HideakiSolutions/axon/issues) — include OS, build output, and the command that failed.

---

## Still stuck?

- [Getting Started](getting-started.md)
- [Troubleshooting](troubleshooting.md)
- [GitHub Discussions](https://github.com/HideakiSolutions/axon/discussions)
