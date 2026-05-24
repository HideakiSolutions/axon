# API Reference — axon MCP Tools

All 26 MCP tools exposed by `axon serve` via stdio JSON-RPC 2.0.

---

## MCP Tools

### `get_context_capsule`

Token-efficient context: pivot files in full + support files skeletonized. Optionally includes past dialogue turns anchored to the same files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | No | Natural language query to guide pivot selection |
| `pivot_files` | string[] | No | Force specific files as pivots |
| `token_budget` | number | No | Max tokens for the capsule (default: 8000) |
| `dialogue_budget` | number | No | Token budget for `related_turns[]` from dialogue history (default: 0 = disabled) |
| `no_cache` | boolean | No | Bypass the query-hash cache and force a fresh assembly (default: false) |

---

### `get_overview`

Top files by coupling degree + most-referenced symbols. Use for onboarding.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `limit` | number | No | Number of results per category (default: 10) |

---

### `get_impact_graph`

Which files depend on (or are depended on by) the given files — bidirectional BFS.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `files` | string[] | **Yes** | File paths to analyze |

---

### `get_callers`

Locate a symbol by name, then return the list of files that import the file defining it.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol_name` | string | **Yes** | Name of the symbol to trace |
| `file_path` | string | No | Narrow to a specific file |
| `limit` | number | No | Max caller files returned (default: 50) |

---

### `get_skeleton`

Signatures-only view (no function bodies) of one or more files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `files` | string[] | **Yes** | File paths to skeletonize |

---

### `get_tests_for`

Test files (by path convention) that import/reference the given files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `files` | string[] | **Yes** | Source files to find tests for |

---

### `search_memory`

Semantic search over saved observations (vector similarity via nomic-embed).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **Yes** | Natural language query |
| `limit` | number | No | Max results (default: 5) |

---

### `save_observation`

Persist a text observation for future retrieval across sessions.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `content` | string | **Yes** | The observation text |
| `tags` | string[] | No | Optional tags for categorization |
| `file_path` | string | No | Associate the observation with a file |

---

### `run_pipeline`

Full project index: parse source files, build dependency graph, compute embeddings.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `root` | string | No | Project root (default: detected from `.git` walk-up) |

---

### `index_paths`

Incremental reindex of specific paths. Used by write-through hooks.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `paths` | string[] | **Yes** | File paths to reindex (empty + `prune=true` sweeps deleted files) |
| `prune` | boolean | No | Remove deleted files from index (default: false) |

---

### `rename`

Graph-assisted rename: find all occurrences of a symbol across the codebase.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol_name` | string | **Yes** | Current symbol name |
| `new_name` | string | **Yes** | New symbol name |
| `dry_run` | boolean | No | If false, writes changes to disk (default: true) |

---

### `route_map`

List all detected HTTP routes with handler files and framework.

> Requires `index_routes = true` in `.axon/config.toml`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `framework` | string | No | Filter by framework (`nextjs`, `express`, `fastapi`, `flask`) |

---

### `api_impact`

Given a route path, return its handler file and the full impact graph of downstream files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `route_path` | string | **Yes** | HTTP route path (e.g. `/api/users/:id`) |

---

### `detect_changes`

Detect which symbols and files are affected by recent git changes.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ref` | string | No | Git ref to diff from (default: `HEAD`) |

---

### `group_list`

List all repos registered in `~/.axon/registry.json` and their group memberships.

No parameters.

---

### `group_impact`

Cross-repo blast radius: given a file path in the current repo, return impacted files in other registered repos.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **Yes** | File path (relative to current project root) |
| `group` | string | No | Limit search to repos in this group |

---

## Dialogue Layer Tools

### `thread_create`

Create a named conversation scope.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | **Yes** | Thread name |
| `kind` | string | No | `project` \| `person` \| `topic` (default: `project`) |

---

### `thread_list`

List all threads.

No parameters.

---

### `thread_get`

List all sessions within a thread, including digest summaries.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `thread_id` | number | **Yes** | Thread ID |

---

### `session_start`

Open a new working session within a thread.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `thread_id` | number | **Yes** | Thread to attach this session to |
| `label` | string | No | Human-readable session label |
| `model` | string | No | Model identifier for this session |

---

### `session_end`

Close a session. Optionally generate and embed an ADF digest for semantic retrieval.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `session_id` | number | **Yes** | Session to close |
| `compute_digest` | boolean | No | Generate ADF digest and embed it (default: false) |

---

### `turn_add`

Append a turn to a session. Auto-anchors to code artifacts found in content.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `session_id` | number | **Yes** | Session to append to |
| `role` | string | **Yes** | `user` \| `assistant` |
| `content` | string | **Yes** | Turn content (auto-anchored to file paths and top-500 symbols) |

---

### `turn_search`

Semantic search over all turns using nomic-embed cosine similarity.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **Yes** | Natural language query |
| `limit` | number | No | Max results (default: 10) |
| `thread_id` | number | No | Scope search to a specific thread |

---

### `session_get`

Retrieve turns for a session in chronological order.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `session_id` | number | **Yes** | Session ID |
| `limit` | number | No | Max turns returned (default: 100) |

---

### `anchor_link`

Manually link a turn to a file or symbol.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `turn_id` | number | **Yes** | Turn to link |
| `file_id` | number | No | File ID to anchor to |
| `symbol_id` | number | No | Symbol ID to anchor to |
| `kind` | string | No | `mentions` \| `decides` \| `questions` (default: `mentions`) |

---

### `dialogue_context`

Retrieve past turns related to files or a semantic query. Used to inject conversation history into `get_context_capsule` when `dialogue_budget > 0`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | No | Semantic query to rank turns |
| `file_paths` | string[] | No | Return turns anchored to these files |
| `limit` | number | No | Max turns (default: 10) |
| `thread_id` | number | No | Scope to a specific thread |

---

## HTTP REST API

When running `axon web` or `axon serve --http`:

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/graph` | File-level graph: `{nodes[], edges[], meta}` |
| `GET` | `/api/graph?mode=symbol` | Symbol-level graph |
| `GET` | `/api/overview` | Top files by coupling + top symbols (mirrors `get_overview`) |
| `GET` | `/api/symbol/<name>` | Symbol detail: `{name, kind, file, line, signature, caller_files}` |
| `GET` | `/api/search?q=<query>` | Search: `{files[], symbols[]}` |
| `GET` | `/api/observations?q=<text>&limit=N` | List observations (semantic search if embeddings enabled) |
| `GET` | `/api/capsule?q=<text>&budget=N&pivots=path1,path2` | Assemble token-budget context capsule |
| `GET` | `/api/metrics` | Request/token/cache/cost aggregates when telemetry is enabled; graph/cache summary otherwise |
| `POST` | `/api/detect-changes` | Detect changed symbols/files (body: `{ref?}`) |
| `GET` | `/api/threads` | List all threads |
| `GET` | `/api/threads/:id/sessions` | List sessions for a thread |
| `GET` | `/api/sessions/:id/turns` | List turns for a session |
| `GET` | `/api/dialogue/search?q=<query>&limit=N` | Semantic search over turns |

### `/api/graph` — file-mode node shape

```json
{
  "id": "src/auth/token.ts",
  "label": "token.ts",
  "language": "typescript",
  "path": "src/auth/token.ts",
  "size": 12,
  "kind": "file",
  "repo": "my-project"
}
```

`repo` field is present only in multi-repo mode (`--all` or `--group`).

### `/api/graph?mode=symbol` — symbol-mode node shape

```json
{
  "id": "1234",
  "label": "fetchGraph",
  "kind": "function",
  "path": "src/api/client.ts",
  "size": 4
}
```

`kind` is one of: `function`, `method`, `class`, `struct`, `interface`, `type`, `namespace`. Edges in symbol mode connect symbol IDs (only edges with both `from_symbol` and `to_symbol` populated are emitted — this requires `granularity = "symbol"` in the source repo).

### `/api/capsule` — response shape

```json
{
  "capsule": {
    "query": "user authentication flow",
    "pivot_files": [
      { "path": "src/auth/token.ts", "content": "...", "is_skeleton": false, "token_estimate": 720 }
    ],
    "support_files": [
      { "path": "src/auth/middleware.ts", "content": "// === verifyToken (function) lines 12-34 ===\n...", "is_skeleton": false, "token_estimate": 180 }
    ],
    "token_estimate": 1840,
    "total_files": 93
  }
}
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LD_LIBRARY_PATH` | — | Only needed for source-tree runs if the binary cannot find DuckDB; release packages set RPATH/RUNPATH |
| `AXON_EMBEDDING_MODEL` | `./models/nomic-embed-text-v1.5.Q4_K_M.gguf` | Path to the embedding model |
| `AXON_DB_PATH` | `.axon/index.duckdb` | Path to the DuckDB index file |
| `AXON_TELEMETRY` | off | Opt into local telemetry with `1`, `true`, `yes`, or `on` |
| `AXON_TELEMETRY_ENDPOINT` | — | Optional HTTP endpoint for best-effort remote telemetry POSTs |
| `AXON_COST_PER_M_INPUT_USD` | `3.0` | Cost basis for `/api/metrics` estimated input cost |

---

## CLI Reference

| Command | Description |
|---------|-------------|
| `axon index [path]` | Walk + parse + insert DB; runs sweep_deleted to purge missing/ignored files |
| `axon index --force` | Force re-resolution of edges/symbols even when file hash is unchanged |
| `axon index-paths <files...> [--prune]` | Incremental reindex of specific paths |
| `axon serve` | MCP stdio JSON-RPC server (default — Claude Code transport) |
| `axon serve --http [--port=N] [--host=H]` | HTTP REST API server |
| `axon serve --http --all` | Aggregate all registered repos into one HTTP graph |
| `axon serve --http --group=<name>` | Aggregate a named group from `~/.axon/registry.json` |
| `axon web [--port=N] [--host=H]` | Browser graph explorer plus HTTP REST API |
| `axon web --all` | Browser graph explorer over all registered repos |
| `axon web --group=<name>` | Browser graph explorer over a named registry group |
| `axon lsp` | Language Server Protocol stdio server for workspace symbols, document symbols, definitions, and references |
| `axon watch [path] [--interval-ms=N] [--debounce-ms=N]` | Portable polling watcher for external edits; reindexes modified files and prunes deletions |
| `axon status` | Show index summary for the current project |

### Project config (`.axon/config.toml`)

```toml
granularity   = "symbol"   # "file" (default) | "symbol" — enables call graph extraction
index_routes  = false      # enable HTTP route detection for route_map / api_impact
fts_enabled   = true       # full-text search index over symbols
telemetry     = false      # opt-in local telemetry; env AXON_TELEMETRY overrides
```

### Ignore patterns (`.axonignore`)

One pattern per line — matched against `path.filename()`:

```
# default skipped: node_modules, .git, target, build, __pycache__,
# .axon, dist, .next, vendor, .venv, venv, .worktrees
third_party
models
```
