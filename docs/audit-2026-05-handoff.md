# v0.5.0 → v0.6.0 Audit Hardening — Session Handoff

Session date: 2026-05-06
Audit plan: `~/.claude/plans/fa-a-uma-an-lise-profunda-idempotent-clarke.md`
Branch: `feature/audit-fixes-v0.5.1` (~22 commits ahead of `develop`)

## What landed this session (v0.5.1)

### Wave 1 — parser coverage (12/12 langs ✅)

All 13 language blocks in `src/parser/parser.cpp` refreshed against the audit:

| Task   | Lang        | New / refined kinds |
|--------|-------------|---------------------|
| W1.T01 | TS/JS       | decorators on classes/methods (docstring), `namespace`, `enum`, `async_function` |
| W1.T02 | Python      | `@deco` lists in docstring, `async_function` |
| W1.T03 | Rust        | `trait`, `enum`, `union`, `module`, `macro`; `impl Trait for T` vs inherent split |
| W1.T04 | Go          | `interface`, `struct`, `type_alias` (was all `type`) |
| W1.T05 | C#          | `property`, `record`, `enum`, `namespace`, `partial_class`, `async_method`, attributes in docstring |
| W1.T06 | PHP         | `namespace`, `trait`, `interface`, `enum`; `#[Attribute]` in docstring |
| W1.T07 | Dart        | `mixin`, `extension`, `enum`, `factory`, async-prefixed kinds |
| W1.T08 | Java        | `record`, `enum`, `annotation_type`, `sealed_class`/`sealed_interface`, `@Annotation` in docstring |
| W1.T09 | Bash        | `variable` for `export`/`readonly`/`declare`/`typeset` |
| W1.T10 | C++         | `enum`, `union`, `friend`; `template<…>` in docstring |
| W1.T11 | Kotlin      | `extension_function`, `suspend_function`, `sealed_class`, `data_class`, `enum_class`, `companion_object`, `type_alias` |
| W1.T12 | Vue         | one-shot stderr warning when `<script>` lacks `lang` attribute |

### Wave 2 — engine quick-wins (4/5 ✅; cache deferred)

- W2.T02 ✅ `.axonignore` gitignore-style globbing (`*`, `**`, `?`, `/anchored`, `dir/`, `!negate`)
- W2.T03 ✅ `pending-writes.txt` 1 MiB cap with flock-safe rotation
- W2.T04 ✅ `AXON_EMBEDDING_MODEL` / `AXON_TOKEN_BUDGET` / `AXON_TELEMETRY` env-var overrides
- W2.T05 ✅ `axon --version` / `-V` / `version` with git SHA via CMake `configure_file`
- W2.T01 ⏳ deferred — capsule cache needs DB schema + JSON serialization + invalidation epoch (separate task)

### Wave 4 — CI/CD partial (2/9 ✅)

- W4.T01 ✅ `build.yml` (ubuntu-22.04 + macos-14 matrix, third_party cache, smoke step)
- W4.T04 ✅ `lint.yml` (shellcheck + clang-format-15 advisory) plus `.clang-format`
- W4.T02/T03/T05/T06/T07/T08/T09 — open (test.yml, sanitizers.yml, hook logging, build-guard `--parallel`, MCP health, EXIT trap)

### Wave 5 — packaging partial (5/10 ✅)

- W5.T01 ✅ clean-state audit: README placeholders genericized, `.dev-squad/` confirmed gitignored
- W5.T02 ✅ `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1)
- W5.T03 ✅ `.github/dependabot.yml` (github-actions weekly + submodules monthly)
- W5.T04 ✅ `release.yml` (tag-driven multi-target tarballs + checksums + auto release notes)
- W5.T07/T08/T09 ✅ examples: `ts-mini`, `python-mini`, `rust-mini` with per-example READMEs
- W5.T10 ✅ multi-stage `Dockerfile` (ubuntu builder → debian:12-slim runtime) + `.dockerignore`
- W5.T05 ⏳ install.sh dependency-detection hardening — open
- W5.T06 ⏳ telemetry HTTP client — env-var plumbing landed; consent UX/endpoint open

### Build + smoke

- `cmake --build build -j 2` → green twice (after W1+W2 batch, then again unchanged across packaging YAML/MD).
- `axon --version` → `axon 0.5.1 (build <sha>)`.
- 12 symbols extracted from a 4-file multi-lang fixture (TS+C#+Dart+Kotlin).
- `.axonignore` with `*.log`, `**/generated/**`, `!keep.log` correctly skips `foo.log` + `generated/skip.ts` while keeping `keep.log`.

## What remains for v0.6.0

### Priority 1 — needed before public release

- **W3 (test foundation)** — zero unit tests across 4,558 LOC of C++ remains the highest-leverage gap. GoogleTest+CTest via FetchContent, fixtures per language under `tests/fixtures/<lang>/`, golden capsule snapshots, `tests/e2e/smoke.sh` exercising `examples/`. ~17 tasks.
- **W4.T02 / T03** — `test.yml` (CTest after build, e2e/smoke), `sanitizers.yml` (ASAN/UBSAN nightly).
- **W5.T05** — `install.sh` robust dependency detection (`jq`, `cmake>=3.20`, `python3`, `git`); embedding-model download with SHA-256 verification.

### Priority 2 — quality, can ship without

- **W2.T01** — capsule cache by query hash. New `capsule_cache` DuckDB table, BLAKE3(query + project_epoch) key, TTL config, `--no-cache` flag.
- **W4.T06** — structured hook logging (`.axon/logs/<hook>.jsonl` + rotation via shared `_log.sh`).
- **W4.T07/T08/T09** — `cmake --parallel N` coverage in build-guard, MCP server health probe in auto-index, EXIT trap for post-edit cleanup.
- **W5.T06** — telemetry HTTP client. The opt-in env var `AXON_TELEMETRY` is already plumbed into `ProjectConfig.telemetry`; what's missing is the actual sender (`src/core/telemetry.{cpp,hpp}`) plus `docs/{en,pt-br}/telemetry.md` describing the consent/payload model.

### Out of scope until v0.6.x+ (W6 roadmap)

- HNSW vector index for sub-millisecond ANN search
- Type-aware `resolve_calls` with overload disambiguation
- File watcher (`inotify`/`FSEvents`) replacing the post-edit hook
- Homebrew tap (`brew install axon`)
- Reproducible benchmark suite for the `−55.5%` token-reduction claim

## How to resume

```bash
git checkout feature/audit-fixes-v0.5.1
git log --oneline develop..HEAD       # confirm the 22-ish commits
cmake --build build --target axon -j 2  # smoke build before adding work
```

The branch is ready for PR review against `develop`. Open the PR via `gh pr create` (human gate per the dev-squad protocol — agents must not push to remote or open PRs unprompted). After merge, tag `v0.5.1` to trigger the new `release.yml`.

## How to resume

```bash
git checkout feature/audit-fixes-v0.5.1
git fetch origin develop
git rebase origin/develop      # only if develop moved
# Pick a task from the table above, edit the relevant parser block,
# commit per-task using conventional-commit style. Reference commits
# 0bca347 / f2162df / 8a0eceb / 2fdde05 for the editing pattern.
```

When the branch is ready for review, open a PR against `develop` — current diff is 1 file × 4 langs in `parser.cpp`, plus `config.{cpp,hpp}`, `main.cpp`, `CMakeLists.txt`, `version.hpp.in`, the post-edit hook, and `CHANGELOG.md`.

## Audit findings index

The full audit (3 explore reports consolidating per-language gaps, hook coverage, packaging readiness) is captured in the conversation transcript that produced the canonical plan; the per-language gap inventory in the plan file remains the durable source.
