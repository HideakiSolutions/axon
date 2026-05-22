# Changelog

All notable changes to axon will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.1.0] — 2026-05-22

### Added

- **Dialogue Layer** — native structured conversation memory in the same DuckDB store used for the code graph. Threads, sessions, turns, anchors, and digests, all locally stored with 768-dim embeddings via nomic-embed-text.
- **Auto-anchor** — on every `turn_add`, axon scans content with a regex for source file paths (13 extensions) and performs word-boundary lookup against the top-500 most-referenced symbols in the dependency graph, automatically linking turns to code artifacts in `turn_anchors`.
- **Axon Digest Format (ADF)** — rule-based session summary generated on `session_end`: `[SESSION:]`, `[ANCHORS:]`, and turn excerpts (first, last, anchored). Embedded with nomic-embed-text for semantic retrieval.
- **`get_context_capsule` extended** — new optional `dialogue_budget` parameter: when > 0, the capsule response includes a `related_turns[]` array of past conversations anchored to the same pivot files, ranked by cosine similarity, within budget. Zero-overhead when omitted.
- **10 new MCP tools** (25 total, up from 15): `thread_create`, `thread_list`, `session_start`, `session_end`, `turn_add`, `turn_search`, `session_get`, `anchor_link`, `dialogue_context`.
- **4 new HTTP REST endpoints**: `GET /api/threads`, `GET /api/threads/:id/sessions`, `GET /api/sessions/:id/turns`, `GET /api/dialogue/search`.
- **`embed_pending_turns`** — mirrors `embed_pending_symbols`; drains turns with NULL embedding on `run_pipeline`, `index_paths`, and the background sync path.
- **New DB tables**: `threads`, `sessions`, `turns`, `turn_anchors` (incremental migration, backwards-compatible).
- **Test suites**: `test_dialogue` (15 tests), `test_objectives` (23 tests), `test_semantic` (9 model-dependent tests, auto-skip in CI).

### Changed

- **DuckDB 1.1.3 → 1.2.2** — fixes a silent bug where `UPDATE` on `FLOAT[N]` columns produced no effect. All embedding UPDATE paths in `dialogue.cpp` and `embeddings.cpp` now use direct `UPDATE`.
- `run_pipeline` and `index_paths` responses now include `turns_embedded` count alongside `symbols_embedded`.

### Bumped

- CMakeLists.txt project version 0.5.12 → 1.1.0 (syncs with git tag series; v1.0.0 was released without a CMakeLists bump)
- DuckDB prebuilt in `release.yml` workflow: 1.1.3 → 1.2.2

## [0.5.12] — 2026-05-19

### Changed

- Remove legacy local runtimes em `.hseos/` — agentes/skills/workflows do projeto agora delegam ao runtime global `enterprise-hseos`. Sem impacto em comportamento do binário; release cut para alinhar tag com working tree e disparar pipeline multi-OS (linux-x64, macos-arm64, windows-x64).

## [0.5.11] — 2026-05-09

### Fixed

- `axon capsule` crashava com `SIGILL` durante `load_tensors` em máquinas sem AVX-512 — o `CPU_REPACK` compila kernels de repack por variante de CPU mas despacha usando a variante mais alta detectada; no CI (Xeon Platinum 8370C, AVX-512) o kernel AVX-512 era selecionado mesmo em máquinas AVX2-only. `GGML_CPU_REPACK=OFF` remove a otimização de repack mas garante execução em qualquer x86-64

### Bumped

- CMakeLists.txt project version 0.5.10 → 0.5.11

## [0.5.10] — 2026-05-09

### Fixed

- `axon capsule` com modelo presente crashava com `SIGILL` em máquinas sem AVX-512 — `libggml-cpu.so` era compilado no runner GitHub Actions (Xeon Platinum 8370C, AVX-512) com instruções ausentes na maioria das máquinas de usuário (AVX2-only). `GGML_AVX512*` agora forçado a `OFF` no CMakeLists.txt

### Bumped

- CMakeLists.txt project version 0.5.9 → 0.5.10

## [0.5.9] — 2026-05-09

### Fixed

- `axon capsule` não crashava mais quando o modelo de embedding está ausente — `std::runtime_error` de `find_model` não era capturado no handler do subcomando, causando `terminate()` + core dump. Agora imprime `[axon] Embedding model not found...` e sai com código 1

### Bumped

- CMakeLists.txt project version 0.5.8 → 0.5.9

## [0.5.8] — 2026-05-09

### Fixed

- `install.sh` detecta corretamente o layout do tarball (script na raiz ao lado de `bin/` e `lib/`) — `AXON_ROOT` apontava para o diretório pai `/tmp` em vez da raiz do pacote
- Staging do `release.yml` agora inclui `hooks/` e `templates/` no tarball — `install.sh` falhava ao tentar copiar `axon-guard.sh` e demais hooks que não estavam empacotados

### Bumped

- CMakeLists.txt project version 0.5.7 → 0.5.8

## [0.5.7] — 2026-05-08

### Fixed

- Distributed tarballs (Linux x64, macOS arm64) agora incluem as shared libraries do llama.cpp (`libllama.so.0`, `libggml*.so.0`) — o `install.sh` funcionava mas o binário falhava ao iniciar por `SONAME` ausente no diretório `lib/`
- `release.yml` staging step: `find build/bin` agora copia todas as `lib*.so*` e `lib*.dylib` junto com `libduckdb`

### Bumped

- CMakeLists.txt project version 0.5.6 → 0.5.7

## [0.5.6] — 2026-05-08

Windows x64 native binary + binary distribution pipeline.

### Added

- **Windows x64 native binary** (`axon-0.5.6-windows-x64.zip`) — DuckDB linked via import lib (`duckdb.lib` + `duckdb.dll`), BLAKE3 portable C fallback (MSVC cannot assemble AT&T-syntax `.S` files)
- **`install.ps1`** — PowerShell installer for Windows: registers Claude Code hooks as `.ps1` files, sets `LD_LIBRARY_PATH` equivalent, configures MCP server in `~/.claude.json`
- **Cross-repo binary publish** — `release.yml` agora publica em `HideakiSolutions/axon-releases` via `RELEASES_PAT` (fine-grained PAT com `contents:write` no repo público); fonte permanece privado

### Fixed

- `release.yml` step "Resolve version" usa `shell: bash` explícito — evita falha no runner `windows-2022` cujo shell padrão é PowerShell (bash parameter expansion `${VAR#prefix}` não existe em pwsh)

### Bumped

- CMakeLists.txt project version 0.5.5 → 0.5.6

## [0.5.5] — 2026-05-07

MCP server cache parity. Extends the v0.5.3 capsule cache from the CLI to the MCP `get_context_capsule` tool — the path Claude Code actually takes when invoking axon as an MCP server.

### Added

- **`get_context_capsule` MCP cache** integration. The handler now consults `capsule_cache_lookup` before assembling, returning the cached payload with `"cache": "hit"` on reuse and `"cache": "miss"` after a fresh assemble. Same key shape as the CLI: `BLAKE3(query + token_budget + project_epoch)`. Cache is bypassed when (a) the new `no_cache` tool argument is `true` or (b) the caller supplies explicit `pivot_files` (explicit pivots steer assembly differently and shouldn't share entries with the implicit semantic-ranking path).
- **Lazy embedding-model check** on the MCP handler. The model-readiness gate moved past the cache lookup, so a server still warming up its embedding model can serve cached queries immediately. Misses still require the model and surface a clear error if it isn't loaded yet.
- **`no_cache` schema field** advertised in the tool's `inputSchema` so MCP clients (Claude Code, axon-web, anything speaking MCP) can opt out per-call.

### Bumped

- CMakeLists.txt project version 0.5.4 → 0.5.5

## [0.5.4] — 2026-05-07

End-to-end smoke harness + a real `.axonignore` glob fix surfaced by it.

### Added

- **`tests/e2e/smoke.sh`** (W3.T17) — drives the user-facing surface that unit tests can't reach: `axon --version`, `axon help`, `axon init/index/status` against `examples/ts-mini`, capsule cache miss → hit → `--no-cache` round-trip, `.axonignore` `*.log` + `**/generated/**` exclusion, `axon skeleton` on `examples/python-mini`. Wired into `build.yml` as the step after `ctest`, so every PR exercises it on Linux + macOS. The capsule-cache section auto-skips with a log when no embedding model is staged on the runner; `AXON_REQUIRE_MODEL=1` flips that into a hard fail for release runs.

### Fixed

- **`.axonignore` `**/generated/**` did not match top-level `generated/x`** — `glob_to_regex` translated the leading `**/` to `.*/`, requiring at least one directory segment before `generated/`. The smoke caught this on first run. Leading `**/` is now translated to `(?:.*/)?` (zero-or-more directories), which matches gitignore semantics. Bare `**` mid-pattern still translates to `.*` as before.

### Bumped

- CMakeLists.txt project version 0.5.3 → 0.5.4

## [0.5.3] — 2026-05-07

Capsule cache + parser test expansion. Closes the last open quick-win from the audit's W2 wave and brings the parser smoke suite to 11 cases.

### Added

- **Capsule cache** by query hash (W2.T01). New `capsule_cache(query_hash, epoch, payload, created_at)` table; key = `BLAKE3(query + token_budget + epoch)` where `epoch = MAX(files.indexed_at)`. Hits skip embedding + ranking + skeletonization. Smoke on a 1-file Python project: 370ms (miss) → 41ms (hit) — **9× speedup**, matching the audit projection. New `--no-cache` flag on `axon capsule` for forced re-assemble. CLI hits emit `"cache": "hit"` for visibility. Best-effort: malformed rows / insert failures log and continue, never throw.
- **Parser smoke tests** for Go, C#, PHP, Dart, C++ (W3.T04/T05/T06/T07/T10). Total parser coverage in CI rises from 6 to 11 tests. The expansion surfaced a real grammar gap — the vendored `tree-sitter-dart` does not emit `mixin_declaration` for top-level `mixin X {}`, even though the W1.T07 handler is wired correctly. Tracked as a v0.6.x grammar-bump task in the handoff.

### Bumped

- CMakeLists.txt project version 0.5.2 → 0.5.3

## [0.5.2] — 2026-05-07

Platform expansion. Adds macOS-arm64 to the build matrix and the release tarball lineup, closing the packaging blocker tracked in `docs/audit-2026-05-handoff.md` post-v0.5.1.

### Added

- **macOS-arm64 build + release**. `.github/workflows/build.yml` and `release.yml` now include `macos-14` / `macos-arm64` targets and download `libduckdb-osx-universal.zip` (DuckDB v1.1.3) into `third_party/duckdb/lib/` before configure. Universal `.dylib` covers both Apple Silicon and Intel Macs.
- **Nightly sanitizers**. New `.github/workflows/sanitizers.yml` runs the test suite under `-fsanitize=address` and `-fsanitize=undefined` matrix on a daily cron + push-to-develop on src/tests/CMake paths. First run on develop landed clean.

### Fixed

- `CMakeLists.txt` `_GLIBCXX_USE_CXX11_ABI=0` is now gated to non-Apple builds. The flag is GCC-libstdc++-specific; forcing it on Apple's libc++ would corrupt inline-namespace mangling.
- `release.yml` rewrites the macOS dylib's install_name to `@rpath/libduckdb.dylib` and patches the axon binary via `install_name_tool` so the published tarball is relocatable.

### Bumped

- CMakeLists.txt project version 0.5.1 → 0.5.2

## [0.5.1] — 2026-05-06

Audit-driven hardening cycle. Output of a deep coverage audit (13 languages × parser blocks × hooks × engine quick-wins × packaging readiness). Closes the highest-impact parser gaps, adds operator-facing knobs, and prepares the project for public distribution.

### Added

**Parser coverage — all 13 languages refreshed (W1)**
- **TypeScript / JavaScript**: decorators on classes/methods captured into docstring; `enum_declaration` → `"enum"`; `internal_module` / `module` → `"namespace"`; `function_declaration` with leading `async` → `"async_function"`.
- **Python**: `decorated_definition` parents walked to capture `@router.get`, `@dataclass`, `@property` etc. into docstring; `async def` → `"async_function"`.
- **Rust**: `trait_item` → `"trait"`, `enum_item` → `"enum"`, `union_item` → `"union"`, `mod_item` → `"module"`, `macro_definition` → `"macro"`. `impl_item` disambiguates `impl Trait for Type` (name = `"Trait for Type"`) vs inherent `impl Type`.
- **Go**: `type_declaration` now classifies via inner spec — `interface_type` → `"interface"`, `struct_type` → `"struct"`, `type_alias` → `"type_alias"`.
- **C#**: `property_declaration` → `"property"`, `record_declaration` → `"record"` (C# 9+), `enum_declaration` → `"enum"`, `namespace_declaration` → `"namespace"`. `partial`/`async` modifiers shift kind to `"partial_class"` / `"async_method"`. `[Attribute]` lists folded into docstring.
- **PHP**: `namespace_definition` → `"namespace"`, `trait_declaration` → `"trait"`, `interface_declaration` → `"interface"`, `enum_declaration` → `"enum"` (PHP 8.1+). `#[Attribute]` lists folded into docstring.
- **Dart**: `mixin_declaration` → `"mixin"`, `extension_declaration` → `"extension"`, `enum_declaration` → `"enum"`, `factory_constructor_signature` → `"factory"`. Async marker on functions/methods promotes kind to async-prefixed.
- **Java**: `record_declaration` → `"record"` (Java 14+), `enum_declaration` → `"enum"`, `annotation_type_declaration` → `"annotation_type"`, `sealed`/`non-sealed` modifier promotes class/interface kind. `@Annotation` lists folded into docstring.
- **Bash**: `declaration_command` with `export`/`readonly`/`declare`/`typeset` emits `kind="variable"`.
- **C++**: `enum_specifier` → `"enum"`, `union_specifier` → `"union"`, `friend_declaration` → `"friend"`. Surrounding `template<…>` parameter list folded into docstring.
- **Kotlin**: extension functions → `"extension_function"`, `suspend` → `"suspend_function"`, `sealed`/`data`/`enum class` modifiers promote kind, `companion_object` → `"companion_object"`, `type_alias` → `"type_alias"`.
- **Vue**: `<script>` without `lang` attribute now emits a one-shot stderr warning (parsing still falls back to JS per spec).

**Engine quick-wins (W2)**
- `axon --version` / `-V` / `version` print version + short git SHA. Wired via CMake `configure_file` populating `src/version.hpp` from `version.hpp.in` at build time.
- Environment variable overrides on top of `.axon/config.toml`, applied in `make_config()`:
  - `AXON_EMBEDDING_MODEL` — absolute or relative path to `.gguf`
  - `AXON_TOKEN_BUDGET` — default capsule token budget
  - `AXON_TELEMETRY` — opt-in flag (1/true/yes/on); off by default
- New TOML keys: `token_budget` (int, default 8000) and `telemetry` (bool, default false).
- `.axonignore` now supports gitignore-style globbing: `*`, `**`, `?`, leading `/` (anchored), trailing `/` (dir-only), `!negation`. Last-rule-wins semantics. Plain-string patterns retain the fast equality path for back-compat.

**Test foundation (W3)**
- GoogleTest+CTest via FetchContent. New `AXON_BUILD_TESTS` CMake option (default ON when top-level). Tests live under `tests/unit/` with the `add_axon_test(name, sources...)` helper from `tests/CMakeLists.txt`. The parser is exposed to tests as an `axon_parser_objs` OBJECT library so unit tests don't drag in DuckDB/llama.cpp.
- `tests/unit/test_parser_smoke.cpp` — first cross-language parser smoke: 6 cases asserting that the W1 audit closures actually emit the new kinds (Rust traits/macros/impl, Python decorators+async, Java records/sealed/annotations, Bash variables, Kotlin sealed/data, TypeScript decorators+namespaces+async). Caught and fixed a TS gap on exported decorated declarations during bootstrap.

**CI / lint (W4)**
- `.github/workflows/build.yml`: ubuntu-22.04 + macos-14 build matrix with third_party caching, ctest run, and post-build `axon --version` / `axon help` smoke.
- `.github/workflows/release.yml`: tag-driven (`v*.*.*`) multi-target binary release. Stages `axon-<version>-<target>.tar.gz` containing `bin/axon`, `lib/libduckdb.{so,dylib}`, README, LICENSE, CHANGELOG, install.sh; generates SHA-256 sidecar; auto-extracts the matching `[X.Y.Z]` slice of CHANGELOG as release notes; creates the GitHub Release via softprops/action-gh-release.
- `.github/workflows/lint.yml`: shellcheck + clang-format-15 (advisory pass via `continue-on-error: true`). New `.clang-format` (LLVM base, 4-space indent, 100-col, c++20).
- README badges refreshed to point at build.yml + lint.yml; CONTRIBUTING.md grew "Running the test suite" + "CI / lint workflows" sections plus a fixture step for new-parser PRs.

**Hook hardening (W4)**
- `axon-build-guard.sh` now covers `cmake --build … --parallel N` alongside the existing `-j` rules (deny on dynamic expansion, on N>cap, on `--parallel` with no number).
- New `scripts/hooks/_log.sh` shared helper. The four hooks (`axon-guard`, `axon-build-guard`, `axon-auto-index`, `axon-post-edit`) source it (silent fallback if absent) and emit one JSON line per invocation to `.axon/logs/<hook>.jsonl` (project-local) or `~/.axon/logs/<hook>.jsonl` (fallback). Live log rotates to `<hook>.<epoch>.jsonl.gz` above `AXON_LOG_MAX_BYTES` (default 5 MiB). Best-effort throughout — every IO branch is `|| true` so a full disk never breaks a hook.

**Packaging for public release (W5)**
- `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1).
- `.github/dependabot.yml` tracking github-actions weekly + git submodules monthly.
- `examples/{ts-mini,python-mini,rust-mini}` — runnable mini-projects exercising the parser surface, each with a README listing expected symbol kinds.
- `Dockerfile` (multi-stage builder + debian:12-slim runtime) plus `.dockerignore` matching the release tarball layout.
- `scripts/install.sh` rewritten for distribution: layout-aware path resolution (accepts both source-tree and release-tarball), `require_cmd` dependency detection with OS-specific install hints (apt/brew), optional embedding model download with interactive prompt and `AXON_DOWNLOAD_MODEL` / `AXON_EMBEDDING_MODEL_URL` / `AXON_EMBEDDING_MODEL_SHA256` environment overrides for unattended installs.

### Fixed

- **`pending-writes.txt`** capped at 1 MiB. The hook (`axon-post-edit.sh`) checks size under the existing flock before each append; above the cap it rotates the live file to `pending-writes.<epoch>.bak` and touches `sync-requested` so the next MCP server tool call does a full BLAKE3-skip walk. Prevents unbounded growth when the server is offline.

### Changed

- README example snippets genericized — `--group=hideakisolutions` and `/opt/hideakisolutions/axon` replaced with `--group=backend` and `/home/alice/projects/axon` placeholders. Email contacts retain the legitimate `hideakiservicos@gmail.com` public maintainer address.

### Out of scope (deferred)

- W2.T01 (capsule cache by query hash) — needs DB schema design + JSON serialization protocol + invalidation epoch logic; sized for a focused follow-up rather than a quick-win commit.
- W3.T02–T17 — per-language test files beyond the shared smoke + golden capsule snapshots + `tests/e2e/smoke.sh`. The W3.T01 foundation is in place; remaining tasks are mechanical fixture writing.
- W4.T03 (sanitizers.yml — ASAN/UBSAN nightly), T08 (MCP health probe in auto-index), T09 (post-edit cleanup EXIT trap — the existing flock-based design already handles partial failures, marking won't-fix-without-evidence).
- W5.T06 — telemetry HTTP client implementation. The opt-in env var (`AXON_TELEMETRY`) is plumbed end-to-end into `ProjectConfig.telemetry`; what remains is the actual sender (`src/core/telemetry.{cpp,hpp}`) plus consent UX docs.

### Bumped

- CMakeLists.txt project version 0.5.0 → 0.5.1

## [0.5.0] — 2026-04-25

Consolidated minor release covering the v0.4.x cycle. No new code beyond v0.4.3 — promotes the cycle's accumulated improvements into a stable minor.

### Highlights since 0.4.0
- HTTP REST: `?mode=symbol`, `/api/observations`, `/api/capsule`
- Symbol-granular capsule rendering — pivots extracted by symbol body, not full file
- Tree-sitter call graph extraction (13 languages) — `kind='calls'` edges with `from_symbol`/`to_symbol` populated
- `axon index --force` flag to re-resolve edges/symbols regardless of file hash
- `.worktrees/` excluded from indexing; `sweep_deleted` purges newly-ignored entries
- Token reduction (5-query sample): 17.615 → 7.846 (−55.5%) end-to-end vs v0.4.0

### Bumped
- CMakeLists.txt project version 0.1.0 → 0.5.0

## [0.4.3] — 2026-04-25

### Added

**Call-graph extraction via tree-sitter**
- Parser now collects `CallSite{caller, callee, line}` for every function/method call inside a function body
- Cross-language coverage: TS/JS/Python/Rust/Go/C#/PHP/Dart/Java/C++/Kotlin
- Caller resolution: smallest enclosing symbol (line-range containment)
- Built-in stop-word filter eliminates control-flow keywords and common built-ins
- New `kind="calls"` edges populate `from_symbol` and `to_symbol` directly
- Unlocks the BFS path in `assemble_capsule` — pivots now expand to their callers (depth=1)

**Tighter caller rendering**
- Pivot symbols (matched by query) get `budget/16` token cap each (full body)
- Caller symbols (BFS depth=1) get `budget/80` cap (signature + 2-3 lines for orientation)
- Conservative `max_symbols=15` keeps the BFS focused

### Fixed
- UTF-8 sanitization on extracted body slices — prevents crash when source contains non-UTF-8 bytes (`json.exception.type_error.316`)

### Impact
- Capsule tokens (avg of 5 queries): 10.125 → 7.846 (−22.5% vs v0.4.1 baseline)
- Files per capsule: 5 → 8-9 relevant (more context, less tokens)
- 378 symbol-to-symbol edges populated in axon's own graph (was 0 in v0.4.2)

## [0.4.2] — 2026-04-25

### Added

**Symbol-granular capsule rendering**
- `assemble_capsule` now uses semantic embedding hits to render only the matched symbols' bodies (signature + docstring + line range), not the whole file
- `bfs_symbols_from_pivots()` in `graph.cpp` traverses `symbol_incoming` (callers) when symbol-level edges are populated; depth-0 (pivot only) still wins quality over file-mode when edges are sparse
- Per-symbol body cap (default `budget/12`) prevents one giant function from consuming the budget; truncated bodies emit a `// … (truncated)` marker
- File-level support fallback when symbol BFS produces little context

**`axon index --force` flag**
- Forces re-parse and re-resolution of edges even when file hash is unchanged
- Required to backfill `from_symbol`/`to_symbol` after enabling `granularity = "symbol"` in `.axon/config.toml`

### Quality
- Pivots now contain focused symbol code instead of full files. Token count similar to file-mode in aggregate but content is much more relevant to the query.
- Headers `// === <name> (<kind>) lines X-Y ===` make it explicit which symbols matched, helping LLMs anchor responses.

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
