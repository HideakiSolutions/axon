# Troubleshooting — axon

---

## Install Problems

### `git submodule update --init --recursive` fails

**Symptom:** Build fails with missing headers (`llama.h`, `tree_sitter/api.h`).

**Diagnosis:**
```bash
git submodule status | grep "^-"
```

**Fix:** Any submodule prefixed with `-` is uninitialized:
```bash
git submodule update --init --recursive
```

If your network blocks GitHub SSH, use HTTPS:
```bash
git config --global url."https://github.com/".insteadOf "git@github.com:"
git submodule update --init --recursive
```

---

### Build fails: `fatal error: llama.h`

**Symptom:** Compiler cannot find llama.cpp headers.

**Fix:** Submodule not initialized (see above). After fixing:
```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j2
```

---

### Build crashes the host / killed by OOM

**Symptom:** Build starts, system slows, process killed.

**Fix:** Use `-j2` instead of `-j$(nproc)`:
```bash
make -j2
```

The llama.cpp + 18 grammar compilation is memory-intensive. `-j2` is the safe maximum for shared hosts.

---

### `cmake` cannot find C++20 compiler

**Symptom:** `CMake Error: The C++ compiler does not support C++20`.

**Fix:**
```bash
sudo apt-get install -y g++-12
cmake .. -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_BUILD_TYPE=Release
```

---

## Runtime Problems

### `axon: error while loading shared libraries: libduckdb.so`

**Symptom:** Binary starts, immediately crashes with shared library error.

**Fix:** release packages should find bundled libraries without `LD_LIBRARY_PATH` (`$ORIGIN/../lib` on Linux, `@executable_path/../lib` on macOS, DLL next to `axon.exe` on Windows). If you are running a source-tree binary directly, set:
```bash
export LD_LIBRARY_PATH=/path/to/axon/third_party/duckdb/lib
```

Add to `~/.bashrc` to persist. Also add to the MCP `env` block in `~/.claude.json`:
```json
"env": { "LD_LIBRARY_PATH": "/path/to/axon/third_party/duckdb/lib" }
```

---

### `axon index` produces 0 symbols

**Symptom:** Index runs but `axon status` shows 0 symbols.

**Diagnosis:**
```bash
axon index /path/to/project --verbose
```

**Common causes:**
- Project root is a subdirectory — axon detects root via `.git` walk-up. Ensure you're indexing the git root.
- All files match an exclude pattern in `.axon/config.toml`.
- Only languages not in the 13 supported are present.

---

### `search_memory` returns no results

**Symptom:** Tool returns empty results even after `save_observation`.

**Cause:** Embedding model not loaded — all embedding columns are NULL.

**Diagnosis:**
```bash
ls /path/to/axon/models/
# Should show: nomic-embed-text-v1.5.Q4_K_M.gguf
```

**Fix:** Download the model:
```bash
pip install huggingface_hub
huggingface-cli download nomic-ai/nomic-embed-text-v1.5-GGUF \
    nomic-embed-text-v1.5.Q4_K_M.gguf \
    --local-dir /path/to/axon/models/
```

---

### `axon serve --http --all` shows only one repo

**Symptom:** `/api/graph` returns nodes from only the current project.

**Diagnosis:**
```bash
cat ~/.axon/registry.json
```

**Fix:** Other repos may not be registered. Run `axon index` in each repo first. The `--all` flag aggregates all repos in `registry.json`.

---

### DuckDB "database is locked" error

**Symptom:** DB-backed MCP tools return a lock error.

**Cause:** Another `axon` process has the DB open in write mode.

Since the peer-proxy mechanism landed, this should be rare: the serve holding
the lock registers itself in `~/.axon/registry.json` (`owner_pid`,
`owner_port`, `owner_token`) and latecomer serves automatically forward tool
calls to it over localhost. A persistent lock error means the proxy path
failed — usually because the lock holder is an older axon binary without a
peer listener, or a non-serve process (e.g. a stuck `axon index`).

`axon filter` and `axon artifact-retrieve` keep working under the lock: the
filter stores CCR artifacts in the `.axon/ccr/` file sidecar when it cannot
open the DB, and retrieval resolves DB → sidecar → lock-holding peer, so
compressed shell output stays recoverable either way.

**Fix:**
```bash
# Find and kill the other process
lsof | grep index.duckdb
kill <pid>
```

Or use a separate database path per mode.

---

## Configuration Problems

### Claude Code shows axon as disconnected

**Step-by-step diagnosis:**

1. Check binary path:
```bash
/path/to/axon/build/axon --version
```

2. Check `~/.claude.json` MCP config:
```json
{
  "mcpServers": {
    "axon": {
      "command": "/absolute/path/to/axon/build/axon",
      "args": ["serve"],
      "env": {
        "LD_LIBRARY_PATH": "/absolute/path/to/axon/third_party/duckdb/lib"
      }
    }
  }
}
```

3. Run axon manually and check for errors:
```bash
LD_LIBRARY_PATH=/path/to/duckdb/lib /path/to/axon/build/axon serve
```

4. Check Claude Code logs: `~/.claude/logs/`

---

### axon MCP tools stop responding after upgrading or killing a serve

**Symptom:** After installing a new axon build (or after an `axon serve`
process was killed), the `mcp__axon__*` tools disappear or error out for the
rest of the session, even though `axon --version` on the command line already
reports the new version.

**Why:** an MCP server is loaded once when the session starts and runs for the
session's lifetime. The binary is memory-mapped at spawn — replacing the file
on disk (or swapping the dist directory) does not touch the running process,
and killing that process does **not** make Claude Code respawn it mid-session.
The client marks the server disconnected and the tools go away until it is
reconnected.

**Fix:** reconnect the MCP server, then the tools return on the current
binary:

- In Claude Code, run `/mcp` and reconnect the `axon` server, **or** start a
  fresh session (a new session spawns the server from the current binary).
- To verify which binary a running serve is on: `ls -l /proc/<pid>/exe` for
  each `pgrep -f "axon serve"` (an inode marked `(deleted)`, or a path under an
  old `axon-dist-backup-*`, means it is a stale process still on the previous
  version).

**Note for the CLI:** the `axon` CLI (`capsule`, `skeleton`, `registry prune`,
`filter`, …) runs the on-disk binary every invocation, so it always reflects
the installed version immediately — only the long-lived MCP/serve process is
pinned to its spawn-time binary.

---

### Write-through hooks not firing

**Symptom:** Files edited in Claude Code are not reindexed.

**Fix:** Re-run the install script:
```bash
bash /path/to/axon/scripts/install.sh /path/to/your-project
```

Check `.claude/settings.json` in your project for `PostToolUse` hooks.

---

## Getting Help

If the above didn't solve your problem:

1. **GitHub Issues:** [github.com/HideakiSolutions/axon/issues](https://github.com/HideakiSolutions/axon/issues)

Include:
- OS and version
- Output of `axon --version`
- Full error message / stack trace
- Steps to reproduce
