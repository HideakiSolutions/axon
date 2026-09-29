# Homebrew tap — published

Status: **published** (owner approved 2026-07-08). The tap repo
`HideakiSolutions/homebrew-axon` predated this draft (created 2026-05 with a
stale 0.5.11 formula) and was updated in place; the canonical formula lives
there — the multi-OS layout (macos-arm64 + linux-x64, wrapper + `axon-setup`)
from the original tap was kept over this draft's arm64-only shape.
`release.yml` carries a `bump-homebrew-formula` job (post-publish convenience,
`continue-on-error` so a failed bump never reddens a good release) that
rewrites version/urls/sha256 on every release using the existing
`RELEASES_PAT` secret — no new token needed.

## Install

```sh
brew tap HideakiSolutions/axon
brew install axon
```

## Notes

- macOS is arm64-only: releases publish a `macos-arm64` tarball. An
  Intel/universal bottle would need a `macos-x64` build job first. Linux x64
  installs via the same formula (`on_linux` branch, LD_LIBRARY_PATH wrapper).
- The tap formula installs the package under `libexec` and wraps the binary —
  dylibs resolve via `@rpath`/`LD_LIBRARY_PATH` against `libexec/lib` (the
  relocatability the RUNPATH fixes in v1.2.1 guarantee).
- The embedding model (~80 MB) stays un-bundled (same choice as the release
  tarball installer): `caveats` points users at `axon-setup`.
- `axon.rb` in this directory is the historical draft, superseded by the tap.
