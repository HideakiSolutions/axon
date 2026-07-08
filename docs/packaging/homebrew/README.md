# Homebrew tap — draft (owner gate pending)

Status: **prepared, not published**. Creating `HideakiSolutions/homebrew-axon`
is an outward-facing action (public repository) and requires explicit owner
confirmation before it happens. Nothing in this directory is consumed by any
build; it is the ready-to-ship draft.

## What ships when the gate is approved

1. **Public repo `HideakiSolutions/homebrew-axon`** containing
   `Formula/axon.rb` (seeded from [`axon.rb`](./axon.rb) in this directory).
   Users then install with:

   ```sh
   brew tap HideakiSolutions/axon
   brew install axon
   ```

2. **Bump automation** — a job appended to `release.yml` that rewrites the
   formula on every release. Proposed job (requires a `TAP_PUSH_TOKEN` secret
   with `contents:write` on the tap repo — store the PAT in `pass` under
   `github/homebrew-axon-tap-push` before configuring the secret):

   ```yaml
   bump-homebrew-formula:
     name: Bump Homebrew formula
     needs: publish
     runs-on: ubuntu-22.04
     steps:
       - uses: actions/checkout@v7
         with:
           repository: HideakiSolutions/homebrew-axon
           token: ${{ secrets.TAP_PUSH_TOKEN }}
       - name: Rewrite url/sha256/version
         run: |
           VERSION="${TAG#v}"
           URL="https://github.com/HideakiSolutions/axon-releases/releases/download/${TAG}/axon-${VERSION}-macos-arm64.tar.gz"
           SHA=$(curl -fsSL "${URL}.sha256" | cut -d' ' -f1)
           sed -i \
             -e "s|^  url .*|  url \"${URL}\"|" \
             -e "s|^  sha256 .*|  sha256 \"${SHA}\"|" \
             -e "s|^  version .*|  version \"${VERSION}\"|" \
             Formula/axon.rb
           git config user.name "axon-release-bot"
           git config user.email "noreply@hideakisolutions.com"
           git commit -am "axon ${VERSION}"
           git push
         env:
           TAG: ${{ github.ref_name }}
   ```

## Notes

- arm64-only for now: releases publish a `macos-arm64` tarball. An
  Intel/universal bottle would need a `macos-x64` build job first.
- The formula symlinks `libexec/bin/axon` into `bin` — the package's dylibs
  resolve via `@rpath` relative to the real binary location, and Homebrew
  resolves symlinks before `@loader_path`, so no relocation surgery is needed
  (this is the same property the RUNPATH fixes in v1.2.1 guarantee).
- The embedding model (~80 MB) stays un-bundled (same choice as the release
  tarball installer): `caveats` points users at the packaged `install.sh`.
