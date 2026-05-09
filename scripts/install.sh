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
#   4. ~/.claude/hooks/axon-build-guard.sh — hook PreToolUse (bloqueia make/ninja com -j alto)
#   5. <project>/.claude/CLAUDE.md         — instruções de uso do axon
#   6. <project>/.claude/settings.json     — registra hooks para Grep, Glob, Bash (build), UserPromptSubmit, PostToolUse

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

cp "$SCRIPTS_DIR/hooks/axon-build-guard.sh" "$HOOKS_DIR/axon-build-guard.sh"
chmod +x "$HOOKS_DIR/axon-build-guard.sh"
echo "[axon] ✓ Hook build-guard: $HOOKS_DIR/axon-build-guard.sh"

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
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '{"hooks": {"PreToolUse": [$grep, $glob, $buildguard], "UserPromptSubmit": [$autoindex], "PostToolUse": [$postedit]}}' > "$SETTINGS"
else
  # Merge: adiciona entradas axon evitando duplicatas
  EXISTING=$(cat "$SETTINGS")
  MERGED=$(echo "$EXISTING" | jq \
    --argjson grep "$GREP_HOOK" \
    --argjson glob "$GLOB_HOOK" \
    --argjson buildguard "$BUILD_GUARD_HOOK" \
    --argjson autoindex "$AUTO_INDEX_HOOK" \
    --argjson postedit "$POST_EDIT_HOOK" \
    '
      .hooks.PreToolUse //= [] |
      .hooks.PreToolUse |= (
        [.[] | select(
          .matcher != "Grep" and
          .matcher != "Glob" and
          (.hooks[0].command | contains("axon-build-guard") | not)
        )] +
        [$grep, $glob, $buildguard]
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
  do_download=0
  if [ "${AXON_DOWNLOAD_MODEL:-}" = "1" ]; then
    do_download=1
  elif [ "${AXON_DOWNLOAD_MODEL:-}" = "0" ]; then
    do_download=0
  elif [ -t 0 ]; then
    read -r -p "[axon] Download embedding model (~150 MiB) to $MODEL_DIR? [y/N] " yn
    [[ "$yn" =~ ^[Yy] ]] && do_download=1
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
    echo "[axon] (skipped model download — set AXON_EMBEDDING_MODEL=<path> or AXON_DOWNLOAD_MODEL=1 later)"
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

echo ""
echo "[axon] Install complete. Restart Claude Code to activate the hooks."
