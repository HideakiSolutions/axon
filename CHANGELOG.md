# Changelog

All notable changes to axon will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.4.1] — 2026-04-25

### Fixed
- **Indexer purges newly-ignored files** — `axon index` now deletes DB entries whose path matches `SKIP_DIRS` or `.axonignore` patterns (previously, ignoring a path only prevented future indexing — already-indexed files lingered)
- `.worktrees/` added to default `SKIP_DIRS` in `indexer.cpp` and `routes.cpp`, eliminating duplicate file entries from git worktrees

### Impact
- Removed 20.096 duplicate worktree nodes across registered repos
- Total graph nodes: 48.805 → 28.709 (−41%)
- Capsule token usage: −43% on average for queries hitting worktree-heavy repos

## [0.4.0] — 2026-04-25

### Added

**Symbol-granular graph mode**
- `GET /api/graph?mode=symbol` — nodes are symbols (functions, classes, methods, interfaces, types, structs, namespaces) instead of files
- Symbol nodes carry `kind` field enabling client-side filtering by symbol type
- Symbol-to-symbol edges derived from edges with `from_symbol`/`to_symbol` populated
- Node size weighted by edge degree; color-coded by kind

**Backend**
- `GET /api/observations?q=<text>&limit=N` — list or semantic-search saved observations
- `GET /api/capsule?q=<query>&budget=N&pivots=<paths>` — assemble context capsule via `assemble_capsule()`
- `/api/graph` edges now expose `from_symbol` and `to_symbol` names via LEFT JOIN

### Changed
- `GET /api/graph` default behavior unchanged (file-level mode); symbol mode opt-in via `?mode=symbol`

---

## [0.3.0] — 2026-04-25

### Added

**Multi-repo registry**
- `~/.axon/registry.json` — global registry with repos and named groups
- Auto-registration on every `axon index` and `axon serve` call
- `group_list` MCP tool — list all registered repos and groups
- `group_impact` MCP tool — cross-repo blast radius for a given file path

**HTTP REST API**
- `axon serve --http [--port=N]` — expose REST API instead of MCP stdio
- `axon serve --http --all` — aggregate all registered repos into one graph response
- `axon serve --http --group=<name>` — aggregate a named group from the registry
- `/api/graph` — returns aggregated nodes + edges as JSON (multi-repo aware)
- `/api/symbol/:id` — symbol detail with callers list
- `/api/search?q=` — full-text + semantic search endpoint
- DuckDB read-only mode for secondary repos (prevents lock conflicts)

**Axon Web** (companion frontend — see `axon-web` repo)
- Interactive force-directed graph via Sigma.js v3 + Graphology
- Per-repo project selector with instant show/hide (no graph rebuild)
- File tree grouped by repo with package icon for each project
- Loading overlay with phase-by-phase progress indicator
- ForceAtlas2 layout runs in Web Worker (non-blocking)

### Fixed

- `detect_changes` missing from `tools_list()` JSON response
- DuckDB aggregation using wrong column names (`from_id`/`to_id` → `from_file`/`to_file`)
- Secondary DB connections failing in read-write mode when primary DB is locked

## [0.2.0] — 2026-04-24

### Added

**New MCP tools (5)**
- `rename` — graph-assisted rename: find all occurrences of a symbol across the codebase and return line-level edits; `dry_run=false` writes to disk
- `route_map` — list all detected HTTP routes with handler files and framework (requires `index_routes=true` in `.axon/config.toml`)
- `api_impact` — given a route path, return its handler file and the full impact graph of downstream files
- `detect_changes` — detect which symbols and files are affected by recent git changes (overlaps diff hunks with symbol line ranges)
- `group_list` / `group_impact` (preview, completed in 0.3.0)

**Core modules**
- `src/core/registry.hpp/cpp` — `RepoEntry`, `RegistryData`, `load_registry()`, `save_registry()`, `register_repo()`
- `src/core/git.hpp/cpp` — git diff parsing via `git diff --unified=0`, hunk extraction
- `src/core/routes.hpp/cpp` — HTTP route detection for Express/FastAPI/Flask/Gin patterns

**CLI**
- `axon serve --http [--port=N]` flag (initial, without multi-repo)
- `axon serve --group=<name>` and `axon serve --all` flags

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
- Semantic exploration, onboarding/vibe coding, refactor impact, debug root cause, test impact, quick inspection, cross-session memory
