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

- installs the Claude Code hooks (Grep/Glob guard, raw shell-output guard, build guard, auto-index, write-through);
- writes `<project>/.claude/settings.json`;
- indexes your project (creates `<project>/.axon/`);
- downloads the embedding model (~80 MB, enables semantic search) - opt out with `AXON_DOWNLOAD_MODEL=0`;
- registers the `axon` MCP server with Claude Code (`claude mcp add-json axon ... --scope user`).

When it finishes, **restart Claude Code** to activate the hooks and the MCP server.

## Embedding device

`AXON_EMBEDDING_DEVICE=auto` (default) uses an available GPU and falls back to
CPU. Set `cpu` to force CPU, or `gpu` to require a GPU and get an explicit error
if none is usable. Set the variable in the environment that starts `axon` or
in the MCP server's `env` configuration. The Linux package uses Vulkan and the
macOS package uses Metal; the Windows package currently supports CPU only.
The selected device appears in the model-loading log.

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
- **A Bash command is denied** -> pipe noisy output through `axon filter`, for example `pytest tests 2>&1 | axon filter test --budget=700 --metrics=json`. For intentional small raw output, prefix the command with `AXON_ALLOW_RAW_SHELL=1`.
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

O instalador instala os hooks, incluindo bloqueio de Grep/Glob e de output Bash
bruto ruidoso, escreve o `settings.json`, indexa o projeto, baixa o modelo de
embeddings (~80 MB; opt-out `AXON_DOWNLOAD_MODEL=0`) e registra o servidor MCP
no Claude Code. Ao terminar, **reinicie o Claude Code**.

**Requisitos:** Linux/macOS precisa de `jq`, `git` e `curl`/`wget`; Windows precisa
do VC++ 2015-2022 Redistributable (x64); e o CLI `claude` no PATH para o registro
automatico do MCP (se ausente, o instalador imprime o bloco para colar no `~/.claude.json`).

**Dispositivo de embeddings:** `AXON_EMBEDDING_DEVICE=auto` (padrao) usa GPU
disponivel e recorre a CPU; `cpu` forca CPU; `gpu` exige GPU e retorna erro
claro se ela nao estiver disponivel. Configure a variavel no processo que
inicia o Axon ou no `env` do servidor MCP. O pacote Linux usa Vulkan, o macOS
usa Metal e o Windows atualmente oferece CPU.
