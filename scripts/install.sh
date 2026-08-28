#!/usr/bin/env bash
# axon install — configura um projeto para usar o axon context engine
#
# Uso: ./scripts/install.sh [project-path]
# Default: diretório atual
#
# O que instala:
#   1. ~/.claude/hooks/axon-guard.sh       — hook PreToolUse global (bloqueia Grep/Glob)
#   2. ~/.claude/hooks/axon-auto-index.sh  — hook UserPromptSubmit (sweep horário de deletados)
#   3. ~/.claude/hooks/axon-post-edit.sh   — hook PostToolUse (write-through síncrono após Write/Edit)
#   4. ~/.claude/hooks/axon-queue-drain.sh — fallback limitado para clientes ociosos
#   5. ~/.claude/hooks/axon-build-guard.sh — hook PreToolUse (bloqueia make/ninja com -j alto)
#   6. ~/.claude/hooks/axon-shell-guard.sh — hook PreToolUse (bloqueia shell output bruto ruidoso)
#   7. <project>/.claude/CLAUDE.md         — instruções de uso do axon
#   8. <project>/.claude/settings.json     — registra hooks para Grep, Glob, Bash, UserPromptSubmit, PostToolUse

set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AXON_ROOT="$(dirname "$SCRIPTS_DIR")"

# Release-tarball layout: install.sh is at the tarball root (alongside bin/ and lib/).
# Detect this by checking for bin/ and lib/ as siblings of the script itself.
# Source-tree layout: install.sh lives in scripts/, so AXON_ROOT is the project root.
if [ -d "$SCRIPTS_DIR/bin" ] && [ -d "$SCRIPTS_DIR/lib" ]; then
  AXON_ROOT="$SCRIPTS_DIR"
fi

AXON_BIN="$AXON_ROOT/build/axon"
# Both the source-tree (third_party/duckdb/lib) and the release-tarball
# layout (lib/) are accepted so the same script ships in both contexts.
if [ -f "$AXON_ROOT/third_party/duckdb/lib/libduckdb.so" ] || \
   [ -f "$AXON_ROOT/third_party/duckdb/lib/libduckdb.dylib" ]; then
  AXON_LIB="$AXON_ROOT/third_party/duckdb/lib"
elif [ -d "$AXON_ROOT/lib" ]; then
  AXON_LIB="$AXON_ROOT/lib"
  AXON_BIN="$AXON_ROOT/bin/axon"
else
  AXON_LIB="$AXON_ROOT/third_party/duckdb/lib"
fi

PROJECT="${1:-$(pwd)}"
PROJECT="$(realpath "$PROJECT")"

HOOKS_DIR="$HOME/.claude/hooks"
CLAUDE_DIR="$PROJECT/.claude"

echo "[axon] Installing for: $PROJECT"

# ── Dependency detection ────────────────────────────────────────────────────
# OS hint per missing tool keeps the failure message useful regardless of
# where the user is — apt for Linux, brew for macOS, scoop/winget hint for
# WSL users coming from PowerShell muscle-memory.
detect_os() {
  case "$(uname -s)" in
    Linux*)  echo linux  ;;
    Darwin*) echo macos  ;;
    *)       echo other  ;;
  esac
}

install_hint() {
  local pkg="$1"
  case "$(detect_os)" in
    linux)  echo "  sudo apt install $pkg     # Debian/Ubuntu" ;;
    macos)  echo "  brew install $pkg         # macOS" ;;
    *)      echo "  Install '$pkg' via your platform's package manager" ;;
  esac
}

require_cmd() {
  local cmd="$1" pkg="${2:-$1}"
  if ! command -v "$cmd" &>/dev/null; then
    echo "[axon] ERROR: '$cmd' not found." >&2
    install_hint "$pkg" >&2
    return 1
  fi
}

missing=0
require_cmd jq       || missing=1
require_cmd git      || missing=1
require_cmd realpath coreutils || missing=1
# cmake + python3 are only needed when building from source. If a pre-built
# binary is staged at $AXON_BIN we skip those checks so the release-tarball
# install path doesn't demand a full toolchain.
if [ ! -x "$AXON_BIN" ]; then
  require_cmd cmake   || missing=1
  require_cmd python3 || missing=1
  require_cmd ninja   || true   # optional — CMake will fall back to make
fi
# Either curl or wget is fine for the embedding model download.
if ! command -v curl &>/dev/null && ! command -v wget &>/dev/null; then
  echo "[axon] ERROR: neither 'curl' nor 'wget' found (needed if the embedding model has to be downloaded)." >&2
  install_hint curl >&2
  missing=1
fi
[ "$missing" = 0 ] || exit 1

# 1. Instalar hooks globais
mkdir -p "$HOOKS_DIR"
cp "$SCRIPTS_DIR/hooks/axon-guard.sh" "$HOOKS_DIR/axon-guard.sh"
chmod +x "$HOOKS_DIR/axon-guard.sh"
echo "[axon] ✓ Hook guard: $HOOKS_DIR/axon-guard.sh"

cp "$SCRIPTS_DIR/hooks/axon-auto-index.sh" "$HOOKS_DIR/axon-auto-index.sh"
chmod +x "$HOOKS_DIR/axon-auto-index.sh"
echo "[axon] ✓ Hook auto-index: $HOOKS_DIR/axon-auto-index.sh"

cp "$SCRIPTS_DIR/hooks/axon-post-edit.sh" "$HOOKS_DIR/axon-post-edit.sh"
chmod +x "$HOOKS_DIR/axon-post-edit.sh"
echo "[axon] ✓ Hook post-edit: $HOOKS_DIR/axon-post-edit.sh"

cp "$SCRIPTS_DIR/hooks/axon-queue-drain.sh" "$HOOKS_DIR/axon-queue-drain.sh"
chmod +x "$HOOKS_DIR/axon-queue-drain.sh"
echo "[axon] ✓ Queue drain fallback: $HOOKS_DIR/axon-queue-drain.sh"

cp "$SCRIPTS_DIR/hooks/axon-build-guard.sh" "$HOOKS_DIR/axon-build-guard.sh"
chmod +x "$HOOKS_DIR/axon-build-guard.sh"
echo "[axon] ✓ Hook build-guard: $HOOKS_DIR/axon-build-guard.sh"

cp "$SCRIPTS_DIR/hooks/axon-shell-guard.sh" "$HOOKS_DIR/axon-shell-guard.sh"
chmod +x "$HOOKS_DIR/axon-shell-guard.sh"
echo "[axon] ✓ Hook shell-guard: $HOOKS_DIR/axon-shell-guard.sh"

# 2. Criar diretório .claude do projeto
mkdir -p "$CLAUDE_DIR"

# 3. Instalar CLAUDE.md
cp "$SCRIPTS_DIR/templates/CLAUDE.md" "$CLAUDE_DIR/CLAUDE.md"
echo "[axon] ✓ CLAUDE.md: $CLAUDE_DIR/CLAUDE.md"

# 4. Criar/atualizar settings.json com hooks para Grep e Glob
SETTINGS="$CLAUDE_DIR/settings.json"
HOOK_CMD="bash $HOOKS_DIR/axon-guard.sh"

AUTO_INDEX_CMD="bash $HOOKS_DIR/axon-auto-index.sh"
POST_EDIT_CMD="bash $HOOKS_DIR/axon-post-edit.sh"
BUILD_GUARD_CMD="bash $HOOKS_DIR/axon-build-guard.sh"
SHELL_GUARD_CMD="bash $HOOKS_DIR/axon-shell-guard.sh"

GREP_HOOK=$(jq -n --arg cmd "$HOOK_CMD" '{
  "matcher": "Grep",
  "hooks": [{"type": "command", "command": $cmd}]
}')

GLOB_HOOK=$(jq -n --arg cmd "$HOOK_CMD" '{
  "matcher": "Glob",
  "hooks": [{"type": "command", "command": $cmd}]
}')

BUILD_GUARD_HOOK=$(jq -n --arg cmd "$BUILD_GUARD_CMD" '{
  "matcher": "Bash",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 5}]
}')

SHELL_GUARD_HOOK=$(jq -n --arg cmd "$SHELL_GUARD_CMD" '{
  "matcher": "Bash",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 5}]
}')

AUTO_INDEX_HOOK=$(jq -n --arg cmd "$AUTO_INDEX_CMD" '{
  "matcher": "",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 5}]
}')

POST_EDIT_HOOK=$(jq -n --arg cmd "$POST_EDIT_CMD" '{
  "matcher": "Write|Edit|MultiEdit|NotebookEdit|Bash",
  "hooks": [{"type": "command", "command": $cmd, "timeout": 10}]
}')

if [ ! -f "$SETTINGS" ]; then
  jq -n \
    --argjson grep "$GREP_HOOK" \
    --argjson glob "$GLOB_HOOK" \
    --argjson buildguard "$BUILD_GUARD_HOOK" \
    --argjson shellguard "$SHELL_GUARD_HOOK" \
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '{"hooks": {"PreToolUse": [$grep, $glob, $buildguard, $shellguard], "UserPromptSubmit": [$autoindex], "PostToolUse": [$postedit]}}' > "$SETTINGS"
else
  # Merge: adiciona entradas axon evitando duplicatas
  EXISTING=$(cat "$SETTINGS")
  MERGED=$(echo "$EXISTING" | jq \
    --argjson grep "$GREP_HOOK" \
    --argjson glob "$GLOB_HOOK" \
    --argjson buildguard "$BUILD_GUARD_HOOK" \
    --argjson shellguard "$SHELL_GUARD_HOOK" \
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '
      .hooks.PreToolUse //= [] |
      .hooks.PreToolUse |= (
        [.[] | select(
          .matcher != "Grep" and
          .matcher != "Glob" and
          (.hooks[0].command | contains("axon-build-guard") | not) and
          (.hooks[0].command | contains("axon-shell-guard") | not)
        )] +
        [$grep, $glob, $buildguard, $shellguard]
      ) |
      .hooks.UserPromptSubmit //= [] |
      .hooks.UserPromptSubmit |= (
        [.[] | select(.hooks[0].command | contains("axon-auto-index") | not)] +
        [$autoindex]
      ) |
      .hooks.PostToolUse //= [] |
      .hooks.PostToolUse |= (
        [.[] | select(.hooks[0].command | contains("axon-post-edit") | not)] +
        [$postedit]
      )
    ')
  echo "$MERGED" > "$SETTINGS"
fi

echo "[axon] ✓ Settings: $SETTINGS"

# 5. Optional embedding model download
# AXON_EMBEDDING_MODEL=<path>          → user provides their own; skip download.
# AXON_DOWNLOAD_MODEL=1 (or 0)         → explicit opt-in/out override.
# Default                              → only download when model is absent
#                                        AND the user is interactive (TTY).
MODEL_DIR="${AXON_MODEL_DIR:-$AXON_ROOT/models}"
DEFAULT_MODEL_NAME="nomic-embed-text-v1.5.Q4_K_M.gguf"
DEFAULT_MODEL_URL="${AXON_EMBEDDING_MODEL_URL:-https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/$DEFAULT_MODEL_NAME}"

if [ -z "${AXON_EMBEDDING_MODEL:-}" ] && [ ! -f "$MODEL_DIR/$DEFAULT_MODEL_NAME" ]; then
  do_download=1
  if [ "${AXON_DOWNLOAD_MODEL:-}" = "0" ]; then
    do_download=0
  elif [ "${AXON_DOWNLOAD_MODEL:-}" = "1" ]; then
    do_download=1
  elif [ -t 0 ]; then
    read -r -p "[axon] Download embedding model (~80 MiB) to $MODEL_DIR? [Y/n] " yn
    [[ "$yn" =~ ^[Nn] ]] && do_download=0
  fi
  if [ "$do_download" = 1 ]; then
    mkdir -p "$MODEL_DIR"
    echo "[axon] Downloading $DEFAULT_MODEL_NAME ..."
    if command -v curl &>/dev/null; then
      curl -fL --progress-bar "$DEFAULT_MODEL_URL" -o "$MODEL_DIR/$DEFAULT_MODEL_NAME"
    else
      wget -q --show-progress "$DEFAULT_MODEL_URL" -O "$MODEL_DIR/$DEFAULT_MODEL_NAME"
    fi
    if [ -n "${AXON_EMBEDDING_MODEL_SHA256:-}" ]; then
      echo "[axon] Verifying SHA-256 ..."
      actual=$(shasum -a 256 "$MODEL_DIR/$DEFAULT_MODEL_NAME" | awk '{print $1}')
      if [ "$actual" != "$AXON_EMBEDDING_MODEL_SHA256" ]; then
        echo "[axon] ERROR: SHA-256 mismatch (expected $AXON_EMBEDDING_MODEL_SHA256, got $actual)" >&2
        rm -f "$MODEL_DIR/$DEFAULT_MODEL_NAME"
        exit 1
      fi
    fi
    echo "[axon] ✓ Model: $MODEL_DIR/$DEFAULT_MODEL_NAME"
  else
    echo "[axon] (skipped model download — set AXON_DOWNLOAD_MODEL=1 or AXON_EMBEDDING_MODEL=<path> to enable later)"
  fi
fi

# 6. Index the project
if [ -x "$AXON_BIN" ]; then
  echo "[axon] Indexing project (this may take a moment)..."
  LD_LIBRARY_PATH="$AXON_LIB" "$AXON_BIN" index "$PROJECT"
  echo "[axon] ✓ Indexed"
else
  echo "[axon] WARN: binary not found at $AXON_BIN — index manually with: axon index $PROJECT"
fi

# 7. Register the MCP server with Claude Code (out-of-the-box usability)
AXON_BIN_ABS="$(realpath "$AXON_BIN" 2>/dev/null || echo "$AXON_BIN")"
AXON_LIB_ABS="$(realpath "$AXON_LIB" 2>/dev/null || echo "$AXON_LIB")"
MODEL_ABS=""
if [ -n "${AXON_EMBEDDING_MODEL:-}" ]; then MODEL_ABS="$AXON_EMBEDDING_MODEL"
elif [ -f "$MODEL_DIR/$DEFAULT_MODEL_NAME" ]; then MODEL_ABS="$MODEL_DIR/$DEFAULT_MODEL_NAME"; fi
MCP_JSON=$(jq -n --arg cmd "$AXON_BIN_ABS" --arg lib "$AXON_LIB_ABS" --arg model "$MODEL_ABS" \
  '{command:$cmd, args:["serve"], env:({LD_LIBRARY_PATH:$lib} + (if $model != "" then {AXON_EMBEDDING_MODEL:$model} else {} end))}')
if command -v claude >/dev/null 2>&1; then
  claude mcp remove axon -s user >/dev/null 2>&1 || true
  if claude mcp add-json axon "$MCP_JSON" -s user >/dev/null 2>&1; then
    echo "[axon] ✓ MCP server registered with Claude Code (user scope)"
  else
    echo "[axon] WARN: could not auto-register the MCP server. Add this to ~/.claude.json:"
    echo "$MCP_JSON" | jq '{mcpServers:{axon:.}}'
  fi
else
  echo "[axon] Claude CLI not on PATH. Add this to ~/.claude.json to enable axon in Claude Code:"
  echo "$MCP_JSON" | jq '{mcpServers:{axon:.}}'
fi

echo ""
echo "[axon] Install complete. MCP server registered. Restart Claude Code to activate the hooks."
