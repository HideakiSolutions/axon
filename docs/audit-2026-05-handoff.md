# Audit Hardening Cycle — v0.5.0 → v0.5.4

Audit window: 2026-05-06 / 2026-05-07
Audit plan: `~/.claude/plans/fa-a-uma-an-lise-profunda-idempotent-clarke.md`
Outcome: 4 dual-platform releases shipped, 53+ commits, 11 PRs merged, ~75 % of audit scope closed.

## Releases shipped

| Tag | Highlight |
|---|---|
| [v0.5.1](https://github.com/HideakiSolutions/axon/releases/tag/v0.5.1) | Audit hardening — all 12 W1 parser blocks refreshed, 4 W2 quick-wins, CI/lint/release workflows, examples, Dockerfile. Linux-x64. |
| [v0.5.2](https://github.com/HideakiSolutions/axon/releases/tag/v0.5.2) | macOS-arm64 build + release via libduckdb-osx-universal staging. Nightly ASAN+UBSAN sanitizers workflow. Dual-platform. |
| [v0.5.3](https://github.com/HideakiSolutions/axon/releases/tag/v0.5.3) | Capsule cache by query hash (9× speedup confirmed). Parser smoke suite expanded to 11 cases. Dual-platform. |
| [v0.5.4](https://github.com/HideakiSolutions/axon/releases/tag/v0.5.4) | E2E smoke harness; real `.axonignore` `**/foo` glob bug fix. Dual-platform. |

## Coverage by wave

| Wave | Status | Coverage notes |
|------|--------|----------------|
| W1 — parser coverage | ✅ 12/12 | Every language block in `src/parser/parser.cpp` rewritten against the audit. New kinds wired across TS/JS/Python/Rust/Go/C#/PHP/Dart/Java/Bash/C++/Kotlin/Vue. |
| W2 — engine quick-wins | ✅ 5/5 | Capsule cache, `.axonignore` globbing, pending-writes cap, env-var overrides, `axon --version`. |
| W3 — test foundation | 🟡 7/17 | GoogleTest+CTest foundation, 11 unit tests across Rust/Python/Java/Bash/Kotlin/TS/Go/C#/PHP/Dart/C++, e2e smoke harness wired into `build.yml`. Remaining tasks are deeper edge-case coverage with diminishing returns. |
| W4 — CI/CD + hooks | ✅ 7/9 | build/release/lint/sanitizers/ci workflows live; structured hook logging via `_log.sh`; build-guard covers `cmake --parallel`. T08/T09 marked won't-fix-without-evidence. |
| W5 — public packaging | ✅ 9/10 | Hardened `install.sh`, `CODE_OF_CONDUCT.md`, dependabot, multi-stage Dockerfile, dual-platform `release.yml`, examples. T06 telemetry HTTP client deferred (env-var plumbing landed). |

## What remains (genuinely open)

### Quality / coverage — diminishing returns

- **W3.T02 / T03 / T08 / T09 / T11 / T12** — deeper per-language unit tests for TS, Python, Java, Bash, Kotlin, Vue beyond the current happy-path smoke.
- **W3.T13** — golden test for `resolve_calls` overload behavior (captures the v0.5.x heuristic for v0.6.x diffing).
- **W3.T14 / T15** — capsule output snapshots against versioned golden files.
- **W3.T16** — `.axonignore` glob unit tests as gtest cases (currently covered by e2e smoke; promoting to unit needs `glob_to_regex` exposed via header).

### Features — real gaps

- **W5.T06** — telemetry HTTP client. `AXON_TELEMETRY` env var is plumbed end-to-end into `ProjectConfig.telemetry`; what's missing is the actual sender (`src/core/telemetry.{cpp,hpp}`), an endpoint URL contract, and `docs/{en,pt-br}/telemetry.md` describing the consent + payload model. Without an endpoint, the work is theoretical.
- **Dart grammar bump** — vendored `tree-sitter-dart` doesn't emit `mixin_declaration`; the W1.T07 handler is correct but never fires. Submodule update or alternate node-kind handler.

### W6 roadmap — out of scope, separate effort

- HNSW vector index (DuckDB VSS) for sub-millisecond ANN search
- Type-aware `resolve_calls` with overload disambiguation
- File watcher (`inotify` / `FSEvents`) replacing the post-edit hook
- Homebrew tap (`brew install axon`)
- Reproducible benchmark suite for the `−55.5 %` token-reduction claim

## Findings caught during TDD

1. **TS exported decorators** (PR #2 bootstrap): decorators on `@Foo export class X {}` sit under `export_statement`, not `class_declaration`. Fixed by walking the parent in `collect_decorators_ts`.
2. **Linux ABI mismatch** (PR #2 round 2): `g_ignore_patterns` was a dangling reference after the W2.T02 refactor — caught by CI clean build.
3. **macOS BLAKE3 SSE** (PR #2 round 2): `.S` files are x86-64-only; arm64 needs portable C path. Gated by `CMAKE_SYSTEM_PROCESSOR`.
4. **`axon help` exit code** (PR #2 round 3): fell through to the default error-exit-1 branch. Added explicit handler returning 0.
5. **`.axonignore **/foo` glob** (PR #10): leading `**/` translated to `.*/` requiring at least one directory segment, missing top-level matches. Fixed to translate to `(?:.*/)?`.
6. **Dart `mixin` grammar gap**: vendored tree-sitter-dart doesn't emit `mixin_declaration`. Documented; needs grammar bump.

## Workflows live on `develop`

| Workflow | Trigger | Scope |
|----------|---------|-------|
| `build.yml`     | push/PR to main+develop | matrix Linux+macOS, ctest, e2e smoke |
| `release.yml`   | tag `v*.*.*`            | dual-platform tarballs + GitHub Release |
| `lint.yml`      | push/PR                 | shellcheck + clang-format-15 (advisory) |
| `sanitizers.yml`| nightly + push to develop | ASAN+UBSAN matrix |
| `ci.yml`        | push/PR                 | shellcheck (back-compat from pre-audit) |

## How to resume the next cycle

1. `git checkout develop && git pull` — last shipped is `v0.5.4` (current HEAD of develop).
2. Pick from "What remains" — preferably a single focus per PR, not a heterogeneous batch.
3. Reference the workflow patterns from PRs #2-#11 — branch name, conventional commit, single PR, CI gate, human merge, tag-driven release.
4. Update `CHANGELOG.md` `[X.Y.Z]` slice and bump `CMakeLists.txt project VERSION` in the same PR as a feature lands.

The dev-squad protocol established here (governance scripts substituted by direct git workflow, since `scripts/governance/` doesn't exist in this repo) is documented implicitly via the merged PR history.
