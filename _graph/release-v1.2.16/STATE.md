# Axon v1.2.16 release state

Status: published and post-publication validated

## Evidence

- Dependency-updated release build: passed at `aefa8e9`.
- CTest after dependency merges: 23/23 passed.
- VS Code: npm clean install, typecheck, bundle and package passed; production audit reports zero vulnerabilities.
- Linux candidate: starts with an empty environment and resolves DuckDB/llama/ggml from its own `lib/` tree.
- Candidate E2E with required real model: 23/23 passed.
- Dependabot PRs #82, #83 and #84 are merged; the post-merge release baseline passed 23/23 tests.
- PR #87 (tag filtering) is merged as `c1672e2`; its required Linux, macOS, Windows, ShellCheck and clang-format checks passed.
- PR #88 (type-aware callees) is merged as `e9a82e6`; its required Linux, macOS, Windows, ShellCheck and clang-format checks passed.
- Integrated `main` at `e9a82e6` builds successfully and passes 24/24 tests.
- Source tag `v1.2.16` is published at the accepted immutable baseline `aefa8e95aa9422468aa69bd431918d4069c5157f`.
- The first tag-triggered run (`33230172242`) exposed a Windows-only release validation defect before publication; no partial release was created.
- PR #89 fixed Windows version validation against the fully staged package and was merged as `36ef0994ce5925d6ea6224876ef7bb02ee1e907a`.
- Corrected release run `33231093433` passed Linux, macOS, Windows, publication and Homebrew formula jobs.
- Public release: `https://github.com/HideakiSolutions/axon-releases/releases/tag/v1.2.16` (14 assets; neither draft nor prerelease).
- Published Linux archive SHA-256: `61aab4ff555651dd4e14877adf4b0b46570b11b3edbed5ebdc14f8feba477d32`.
- Published macOS archive SHA-256: `e0aab221aeaf8a1680fb154fc18a0617f3dc18c313ce7a47fe420f1091044515`.
- Published Windows archive SHA-256: `7a84a5637160e2f1cc2cd6cc78bb0bc15335632ff31c7b4d761fea69f649b14f`.
- Published VSIX SHA-256: `c0dbb16d62c7a516e9e5d80a519401f9807caba82af58dd73d7c434dc67ae231`.
- Post-publication smoke downloaded the public Linux archive and VSIX, verified both checksums and archive integrity, rejected no unsafe tar paths, and ran the extracted binary in an empty environment as `axon 1.2.16 (build aefa8e9)`.
- Homebrew formula now declares version `1.2.16` with the published Linux and macOS URLs and checksums.

## Gates

- Publication was explicitly authorized by the user on 2026-08-29.
- PR #87 (tag filtering), PR #88 (type-aware callees), and PR #89 (release workflow fix) remain after the immutable `v1.2.16` source baseline and are not included in its product payload.
- The source tag remains fixed at `aefa8e95aa9422468aa69bd431918d4069c5157f`; the workflow correction was used from `main` only to safely rebuild and publish that tag.
