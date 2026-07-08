# Draft formula for the HideakiSolutions/homebrew-axon tap (NOT yet published —
# creating the public tap repo is owner-gated; see README.md in this directory).
# On release, automation rewrites `url`, `sha256`, and `version` (see the
# proposed release.yml job in README.md).
class Axon < Formula
  desc "Local-first context engine and agentic memory for AI coding agents"
  homepage "https://github.com/HideakiSolutions/axon"
  url "https://github.com/HideakiSolutions/axon-releases/releases/download/v1.2.9/axon-1.2.9-macos-arm64.tar.gz"
  sha256 "c37f00158c3320cab14d748eada063c178521f47844e9b0fc15a4a8be5ba2aad"
  license "MIT"
  version "1.2.9"

  depends_on arch: :arm64
  depends_on :macos

  def install
    # The release package is relocatable (dylibs resolved via @rpath relative
    # to the binary), so install it whole and symlink the binary — a symlink
    # is resolved to its target before @loader_path is computed.
    libexec.install Dir["*"]
    bin.install_symlink libexec/"bin/axon"
  end

  def caveats
    <<~EOS
      The semantic-search embedding model (~80 MB) is not bundled. Download it
      into ~/.axon/models/ or run the packaged installer once:
        #{libexec}/install.sh
      To register the MCP server with Claude Code:
        claude mcp add-json axon '{"command":"#{opt_bin}/axon","args":["serve"]}' --scope user
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/axon --version")
    assert_match "Context Engine", shell_output("#{bin}/axon help")
  end
end
