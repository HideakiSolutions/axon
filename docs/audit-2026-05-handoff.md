# v0.5.0 → v0.6.0 Audit Hardening — Session Handoff

Session date: 2026-05-06
Audit plan: `~/.claude/plans/fa-a-uma-an-lise-profunda-idempotent-clarke.md`
Branch: `feature/audit-fixes-v0.5.1` (8 commits ahead of `develop`)

## What landed this session (v0.5.1)

| Commit  | Task     | Area                                         |
|---------|----------|----------------------------------------------|
| fde49a4 | W2.T05   | `axon --version` / `-V` with git SHA         |
| 5cf5b00 | W2.T04   | `AXON_EMBEDDING_MODEL`/`TOKEN_BUDGET`/`TELEMETRY` env vars |
| 3ff02f6 | W2.T03   | `pending-writes.txt` 1 MiB cap with rotation |
| 0bca347 | W1.T03   | Rust traits, enums, unions, modules, macros; split `impl Trait for T` from inherent impl |
| f2162df | W1.T02   | Python decorators (`@router.get`, `@dataclass`) into docstring; `async_function` kind |
| 8a0eceb | W1.T08   | Java records, sealed classes/interfaces, enums, annotation types; `@Annotation`s into docstring |
| 2fdde05 | W1.T09   | Bash `export`/`readonly`/`declare` variable symbols |
| 8543d82 | docs     | CHANGELOG `[0.5.1]` section                  |

Build: `cmake --build build -j 2` → green.
Smoke: 18 symbols extracted from a 4-file multi-lang fixture (Python+Rust+Java+Bash); `axon --version` returns `axon 0.5.1 (build <sha>)`; `AXON_TOKEN_BUDGET=4096 axon index` honors env override.

## Remaining audit findings (8 parser langs + 2 engine + W3-W5)

Pick up from this branch (or rebase fresh from `develop` if other work landed).

### Wave 1 — parser blocks still on the original v0.5.0 implementation

| Task   | Lang        | Section in `src/parser/parser.cpp` | Headline gaps                                                              |
|--------|-------------|------------------------------------|----------------------------------------------------------------------------|
| W1.T01 | TS/JS       | ~313–356                            | decorators (TS), namespaces, generics in signature, JSX/TSX components, `.d.ts` ambient flag, `import()` dynamic |
| W1.T04 | Go          | ~586–604                            | `interface_type_element`, generics 1.18+ type parameter list               |
| W1.T05 | C#          | ~402–431                            | `property_declaration`, `attribute_list`, `partial`/`async` flags          |
| W1.T06 | PHP         | ~433–458                            | `namespace_definition`, `trait_declaration`, PHP 8 `attribute_list`        |
| W1.T07 | Dart        | ~460–483                            | `mixin_declaration`, `extension_declaration`, `factory_constructor_signature`, async flag |
| W1.T10 | C++         | ~630–672                            | `template_declaration` in signature, overload `#N` suffix, `friend_declaration`, header vs impl split |
| W1.T11 | Kotlin      | ~674–704                            | extension functions, `sealed class`, `companion_object`, top-level flag    |
| W1.T12 | Vue         | ~516–584                            | `<template>` parsing, `<style>` ignored deliberately, warn when `lang` attr absent |

**Pattern to copy from this session:** the Rust/Python/Java commits are the reference. Each lang got: new symbol kinds in the existing `if (kind == ...)` chain, a small `collect_<modifier>` lambda for decorators/annotations, and back-compat retained on legacy kinds (`struct_item` stayed `kind="class"` to preserve existing edge queries).

### Wave 2 — engine quick-wins still open

- **W2.T01 — capsule cache by query hash.** New table `capsule_cache(query_hash BLAKE3, payload BLOB, created_at TIMESTAMP)` in `src/core/db.cpp`; cache key = BLAKE3(query + "|" + project_epoch). TTL via project config; `--no-cache` flag on the capsule subcommand. Hit ratio target ≥10× speedup for repeated queries.
- **W2.T02 — `.axonignore` gitignore-style globbing.** Replace the filename-only equality at `src/core/indexer.cpp:20-48` with a recursive matcher supporting `*`, `**`, `?`, leading `/`, and `!negation`. New `src/core/glob.{cpp,hpp}`. Keep back-compat for plain-name patterns.

### Waves 3–5 — not started

- **W3 (test foundation)** — GoogleTest+CTest setup, fixtures per language, golden capsule snapshot tests, `tests/e2e/smoke.sh`. Audit confirmed zero unit tests in 4,558 LOC of C++. This is the highest-leverage wave for safety as the project goes public.
- **W4 (CI/CD + sanitizers)** — `.github/workflows/{build,test,sanitizers,lint,release}.yml`, ASAN/UBSAN nightly, `.clang-format`/`.clang-tidy`. Hooks gain structured `.axon/logs/<hook>.jsonl` with rotation; `axon-build-guard.sh` to cover `cmake --parallel N`.
- **W5 (public release packaging)** — clean-state audit, `CODE_OF_CONDUCT.md`, `.github/dependabot.yml`, release.yml with multi-platform binaries, hardened install.sh with model download, opt-in telemetry client (env var already wired this session), `examples/{ts-mini,python-mini,rust-mini}`, multi-stage Dockerfile. The W6 roadmap (HNSW, type-aware `resolve_calls`, file watcher) stays out of scope — separate effort.

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
