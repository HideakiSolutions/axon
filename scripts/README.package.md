# axon - Context Engine for AI Coding Agents

This package contains a prebuilt `axon` plus an installer. You do **not** need a
compiler or to build anything - just extract and run the installer.

## Install (Linux / macOS)

```bash
./install.sh /path/to/your-project
```

## Install (Windows, PowerShell)

```powershell
.\install.ps1 C:\path\to\your-project
```

That is all. The installer:

- installs the Claude Code hooks (Grep/Glob guard, build guard, auto-index, write-through);
- writes `<project>/.claude/settings.json`;
- indexes your project (creates `<project>/.axon/`);
- downloads the embedding model (~80 MB, enables semantic search) - opt out with `AXON_DOWNLOAD_MODEL=0`;
- registers the `axon` MCP server with Claude Code (`claude mcp add-json axon ... --scope user`).

When it finishes, **restart Claude Code** to activate the hooks and the MCP server.

## Requirements

- **Linux/macOS:** `jq`, `git`, and `curl` or `wget` (e.g. `sudo apt install jq git curl`).
- **Windows:** the **Visual C++ 2015-2022 Redistributable (x64)** - the installer detects it and opens the download if it is missing (https://aka.ms/vs/17/release/vc_redist.x64.exe).
- The **Claude Code CLI** (`claude`) on your PATH for automatic MCP registration. If it is not found, the installer prints the exact block to paste into `~/.claude.json`.

## Verify

```bash
bin/axon --version          # Linux/macOS (Windows: bin\axon.exe --version)
claude mcp get axon         # confirms the MCP server is registered
```

## Troubleshooting

- **`jq: not found`** -> install `jq` and re-run the installer.
- **MCP not registered** -> the installer printed an `mcpServers` block; paste it into `~/.claude.json`, or run the shown `claude mcp add-json` command.
- **Semantic search says "Embedding model not loaded"** -> the model was not downloaded; re-run with `AXON_DOWNLOAD_MODEL=1 ./install.sh /path/to/your-project`.

---

# axon - PT-BR

Este pacote traz um `axon` pre-compilado e um instalador. Voce **nao** precisa
compilar nada - basta extrair e rodar o instalador.

## Instalar (Linux / macOS)

```bash
./install.sh /caminho/para/seu-projeto
```

## Instalar (Windows, PowerShell)

```powershell
.\install.ps1 C:\caminho\para\seu-projeto
```

O instalador instala os hooks, escreve o `settings.json`, indexa o projeto, baixa
o modelo de embeddings (~80 MB; opt-out `AXON_DOWNLOAD_MODEL=0`) e registra o
servidor MCP no Claude Code. Ao terminar, **reinicie o Claude Code**.

**Requisitos:** Linux/macOS precisa de `jq`, `git` e `curl`/`wget`; Windows precisa
do VC++ 2015-2022 Redistributable (x64); e o CLI `claude` no PATH para o registro
automatico do MCP (se ausente, o instalador imprime o bloco para colar no `~/.claude.json`).
