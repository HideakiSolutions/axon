# Changelog

All notable changes to axon will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2026-04-23

### Added

**MCP tools (10 total)**
- `get_context_capsule` — token-efficient context capsule: pivot files complete + support files skeletonized
- `get_overview` — onboarding/vibe coding: top files by coupling degree + most-referenced symbols
- `get_impact_graph` — bidirectional BFS: which files depend on a given set of files
- `get_callers` — backward trace (file-granular): which files import the file defining a symbol
- `get_skeleton` — signatures-only view of one or more files (no function bodies)
- `get_tests_for` — test impact: test files (by path convention) that import the given files
- `search_memory` — semantic search over saved observations (cross-session)
- `save_observation` — persist an insight for future retrieval
- `run_pipeline` — full project index (parse + dependency graph + embeddings)
- `index_paths` — incremental reindex of specific paths (write-through)

**Language parsers (13 via tree-sitter)**
- TypeScript, JavaScript, Python, Rust, Go, C#, PHP, Dart, Java, Bash, C++, Kotlin, Vue (SFC with TS/JS sub-parse)

**Storage & embeddings**
- DuckDB embedded storage (schema: files, symbols, edges, observations)
- llama.cpp integration with nomic-embed-text-v1.5 (dim=768) for semantic search
- BLAKE3 hashing for incremental reindex (skip unchanged files)

**Claude Code integration**
- Write-through sync via PostToolUse hooks (Edit/Write/Bash covered)
- Build guard hook enforcing `-j2` to protect shared dev hosts
- `scripts/install.sh` — idempotent installer that wires axon into any Claude Code project
- `scripts/templates/CLAUDE.md` — agentic workflow guide injected into projects
- `.claude/settings.json` with pre-approved MCP tool permissions

**Agentic workflow coverage**
- Semantic exploration, onboarding/vibe coding, refactor impact, debug root cause, test impact, quick inspection, cross-session memory — all routed through MCP tools (no Grep/Glob needed)
