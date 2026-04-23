# axon — Context Engine for AI Coding Agents

> *O axônio transmite sinais de forma eficiente entre neurônios. Axon faz o mesmo para agentes de IA.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CI](https://github.com/HideakiSolutions/axon/actions/workflows/ci.yml/badge.svg)](https://github.com/HideakiSolutions/axon/actions/workflows/ci.yml)
[![Claude Code](https://img.shields.io/badge/Claude%20Code-ready-blue)](https://docs.anthropic.com/claude-code)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](CMakeLists.txt)

A local MCP server in C++20 that delivers **surgical context** for AI coding agents — reducing token consumption by **76–98%** compared to dumping full files.

## How it works

```
your project → [AST parse] → [dependency graph] → [embeddings] → DuckDB
                                                                      ↓
  Claude ← [~8k token capsule] ← [BFS + skeletonize] ← [semantic search]
```

**Pivot files** (most relevant to the query) arrive complete.  
**Support files** (nearby dependencies) arrive as signatures only.  
Result: a context capsule that fits your token budget with zero information loss on the critical path.

## Token reduction

Measured on real projects:

| Project | Language | Without axon | With axon | Reduction |
|---------|----------|-------------|-----------|-----------|
| event-platform | TypeScript | 37,509 | 8,774 | **76%** |
| poynt-hub | TypeScript | 17,949 | 1,516 | **91%** |
| mcp-factory | Python | 22,480 | 1,389 | **93%** |
| event-platform | Python | 42,546 | 2,353 | **94%** |

At 1,000 calls/day with a typical TypeScript project (Claude Sonnet — $3/M input tokens):

| | Without axon | With axon | Savings |
|--|-------------|-----------|---------|
| Tokens/call | 37,509 | 8,774 | −76% |
| Cost/day | $112.50 | $26.32 | **−$86/day** |
| Cost/month | $3,375 | $790 | **−$2,585/month** |

## MCP tools

| Tool | Parameters | Description |
|------|-----------|-------------|
| `get_context_capsule` | `query`, `pivot_files?`, `token_budget?` | Token-efficient context capsule |
| `get_overview` | `limit?` | Onboarding/vibe coding: top files by coupling + top symbols |
| `get_impact_graph` | `files[]` | Which files depend on the given files |
| `get_callers` | `symbol_name`, `file_path?`, `limit?` | Files that import the file defining a symbol |
| `get_skeleton` | `files[]` | Signatures-only view (no function bodies) |
| `get_tests_for` | `files[]` | Test files (by path convention) that import the given files |
| `search_memory` | `query`, `limit?` | Semantic search over saved observations |
| `save_observation` | `content`, `tags?`, `file_path?` | Persist an insight for future retrieval |
| `run_pipeline` | `root?` | Full project index (parse + graph + embeddings) |
| `index_paths` | `paths[]`, `prune?` | Incremental reindex of specific paths |

### Agentic workflow coverage

| Dev flow | Canonical tool |
|----------|---------------|
| Semantic exploration | `get_context_capsule` |
| Onboarding / vibe coding | `get_overview` → `get_context_capsule` |
| Before refactor | `get_impact_graph` + `get_tests_for` |
| Debug / root cause | `get_callers` → `get_skeleton` → `get_context_capsule` |
| Quick structure inspection | `get_skeleton` |
| Cross-session memory | `search_memory` / `save_observation` |

## Supported languages (13)

| Language | Extensions |
|----------|-----------|
| TypeScript | `.ts`, `.tsx` |
| JavaScript | `.js`, `.jsx`, `.mjs`, `.cjs` |
| Python | `.py` |
| Rust | `.rs` |
| Go | `.go` |
| C# | `.cs` |
| PHP | `.php` |
| Dart | `.dart` |
| Java | `.java` |
| Bash | `.sh`, `.bash` |
| C++ | `.cpp`, `.cc`, `.cxx`, `.hpp`, `.h` |
| Kotlin | `.kt`, `.kts` |
| Vue | `.vue` (SFC with TS/JS sub-parse) |

## Quick start

### Prerequisites

```bash
sudo apt-get install -y build-essential cmake git ccache
```

### Build

```bash
git clone --recurse-submodules https://github.com/HideakiSolutions/axon.git
cd axon && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j2
```

> **Note:** Use `-j2` (maximum 2 parallel jobs). Higher parallelism during the llama.cpp + 13 tree-sitter grammar compilation can lock up a shared development host. First build takes ~10–12 min; subsequent builds with `ccache` are ~70% faster.

### Embedding model (optional — enables semantic search)

```bash
pip install huggingface_hub
huggingface-cli download nomic-ai/nomic-embed-text-v1.5-GGUF \
    nomic-embed-text-v1.5.Q4_K_M.gguf \
    --local-dir ./models/
```

Without the model, all tools work normally except `search_memory` and semantic-query mode of `get_context_capsule`.

### Configure Claude Code

Add to `~/.claude.json` under `mcpServers`:

```json
{
  "mcpServers": {
    "axon": {
      "command": "/path/to/axon/build/axon",
      "args": ["serve"],
      "env": {
        "LD_LIBRARY_PATH": "/path/to/axon/third_party/duckdb/lib"
      }
    }
  }
}
```

### Index your project and use

```bash
export LD_LIBRARY_PATH=/path/to/axon/third_party/duckdb/lib

axon index /path/to/your-project    # parse AST + build graph + embed
axon status                          # index statistics
axon serve                           # start MCP server (stdio)
```

### Install axon into a Claude Code project

```bash
bash /path/to/axon/scripts/install.sh /path/to/your-project
```

This wires up write-through hooks, the build guard, and injects the agentic workflow guide into the project's `.claude/CLAUDE.md`.

## Architecture

```
src/
├── main.cpp              # CLI: index | serve | capsule | skeleton | status
├── core/
│   ├── config.hpp/cpp    # Project root detection via .git walk-up
│   ├── db.hpp/cpp        # DuckDB connection + schema + migrations
│   ├── indexer.hpp/cpp   # File walk → parse → insert DB (2-pass: files then edges)
│   ├── graph.hpp/cpp     # In-memory adjacency list + bidirectional BFS
│   ├── capsule.hpp/cpp   # Pivot selection + token-budget-aware context assembler
│   ├── skeleton.hpp/cpp  # Signatures-only view via tree-sitter AST
│   └── embeddings.hpp/cpp # llama.cpp wrapper (embed + batch embed)
├── parser/
│   └── parser.hpp/cpp    # Language dispatcher + symbol/import extraction (13 langs)
└── mcp/
    ├── server.hpp/cpp    # stdio JSON-RPC 2.0 loop + write-through drain
    └── protocol.hpp      # make_response / make_error / make_tool_result helpers
third_party/
├── duckdb/               # Pre-built shared library v1.1.3
├── llama.cpp/            # Submodule — inference engine for embeddings
├── tree-sitter/          # Core C API
├── tree-sitter-{lang}/   # 13 language grammar submodules
├── blake3/               # Fast file hashing (incremental reindex)
└── nlohmann-json/        # Header-only JSON
```

## Database schema

```sql
files        (id, path, language, hash, byte_size, indexed_at)
symbols      (id, file_id, name, kind, start_line, end_line, signature, docstring, embedding FLOAT[768])
edges        (id, from_file, to_file, from_symbol, to_symbol, kind)
observations (id, content, file_path, embedding FLOAT[768], created_at)
```

## Roadmap

- [ ] File watcher (inotify/FSEvents) for reindex on edits outside Claude Code
- [ ] Symbol-granular edges (populate `from_symbol`/`to_symbol`) — improves `get_callers` precision
- [ ] HNSW vector index (DuckDB VSS) for projects > 100k symbols
- [ ] Filtered tags in `search_memory`
- [ ] Capsule cache by query hash

## License

MIT — see [LICENSE](LICENSE).
