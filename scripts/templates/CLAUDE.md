# Axon Context Engine

Este projeto está indexado pelo axon. Toda exploração de código roteia por MCP tools do axon; Grep/Glob e shell output bruto ruidoso são bloqueados por hook. Garantia: 76-98% menos tokens sem perda de qualidade.

## Linguagens suportadas (13)

TypeScript, JavaScript, Python, Rust, Go, C#, PHP, Dart, Java, **Bash**, **C++**, **Kotlin**, **Vue** (SFC com sub-parse TS/JS).

## Tools MCP — catálogo e quando usar

| Tool | Uso canônico |
|---|---|
| `get_overview(limit?)` | **Repo desconhecido / vibe coding.** Top files por acoplamento + top símbolos referenciados. É a primeira coisa a chamar quando não há query formada. |
| `get_context_capsule(query, pivot_files?, token_budget?)` | **Query semântica.** Entry point padrão quando você já sabe o que procurar. Retorna pivots completos + support files skeletonizados. |
| `get_impact_graph(files)` | **Antes de editar.** Quais arquivos dependem dos arquivos dados. |
| `get_callers(symbol_name, file_path?, limit?)` | **Debug / root cause.** Backward trace file-granular: dado um símbolo, retorna os arquivos que importam o arquivo-definidor. Narrowing com `get_skeleton(caller_files)` para chegar a call sites. |
| `get_tests_for(files)` | **Test impact.** Testes (por convenção de path) que importam os arquivos dados. Use antes de mergear. |
| `get_skeleton(files)` | **Inspeção rápida.** Só assinaturas, sem corpos de função. |
| `search_memory(query, limit?)` | **Memória cross-session.** Recupera observações salvas. |
| `save_observation(content, tags?, file_path?)` | **Persistir insight.** Chame após descoberta arquitetural não-óbvia. |
| `index_paths(paths, prune?)` / `run_pipeline(root?)` | **Reset.** Normalmente desnecessário — write-through é automático via hooks. Use só se o índice parecer corrompido ou após operações em massa fora do Claude Code. |

## Fluxos agentic — exemplos end-to-end

### Onboarding em repo desconhecido
```
get_overview(limit=10)
  → escolher pivot de interesse a partir dos top files/symbols
  → get_context_capsule(query="<pergunta derivada dos entry points>")
  → save_observation(content="<mapa mental>", tags=["overview", "<projeto>"])
```

### Refactor cross-file
```
get_context_capsule(query="<feature a refatorar>")
  → get_impact_graph(files=[arquivos-pivot])
  → get_tests_for(files=[arquivos-pivot])
  → editar (write-through reindexa automaticamente)
  → rerodar testes retornados
```

### Debug / regressão
```
get_callers(symbol_name="<função suspeita>")
  → get_skeleton(files=[caller_files])   # narrowing para call sites reais
  → get_context_capsule(query="<hipótese>", pivot_files=[callers relevantes])
  → save_observation(content="<root cause>", tags=["bug", "<módulo>"])
```

### Test impact antes de mergear
```
get_tests_for(files=[arquivos editados])
  → rodar os testes retornados
  → se ausentes, get_impact_graph para achar testes indiretos
```

## Write-through garantido

Hook PostToolUse (`axon-post-edit.sh`) cobre toda mudança no filesystem e o MCP server reconcilia antes de cada tool call — **não é preciso chamar `run_pipeline` entre edições**:

- **Write / Edit / MultiEdit / NotebookEdit** → path entra em `.axon/pending-writes.txt`; drain re-indexa + embeda na próxima tool call.
- **Bash** (rm, mv, git checkout, scripts) → toca `.axon/sync-requested`; walk + BLAKE3-skip + prune detecta deletes/renames/gerados.

## Build constraints — sempre `-j2`

Hook PreToolUse `axon-build-guard.sh` bloqueia `make/cmake/ninja` com `-j>2`, `-j$(nproc)`, `-j` sem número e `ninja` sem `-j` explícito.

Motivo: host de desenvolvimento roda simultaneamente MCP server axon, embeddings llama.cpp, segundo cérebro e outras sessões Claude. Paralelismo alto (llama.cpp + 13 tree-sitter grammars) trava a máquina.

Aceitos: `make -j2`, `cmake --build build -j 2`, targets específicos (`make -j2 axon`). Acelerador: `ccache` corta rebuild ~70%.

Escape (casos justificados, ex: primeira build em máquina ociosa):
```bash
AXON_ALLOW_HIGH_PARALLELISM=1 make -j8
```

## Shell output — sempre filtrado

Hook PreToolUse `axon-shell-guard.sh` bloqueia comandos Bash conhecidos por despejar muito texto sem métrica/CCR:

- busca: `rg`, `grep`, `ack`, `ag`
- diffs: `git diff` completo
- leituras brutas de source/docs: `cat`, `sed`, `awk`, `nl`
- testes/build diagnostics: `pytest`, `vitest`, `ctest`, `gtest`, `tsc`
- lint/package/logs: `eslint`, `ruff`, `prettier`, `npm install`, `journalctl`, `docker logs`, `kubectl logs`

Use MCP para contexto de código:
```text
get_context_capsule(query="...", pivot_files=[...], token_budget=...)
get_skeleton(files=[...])
```

Use filtros Axon quando precisar rodar o comando:
```bash
git diff -- src | axon filter diff --budget=600 --metrics=json
rg -n "symbol" src | axon filter grep --budget=600 --metrics=json
pytest tests 2>&1 | axon filter test --budget=700 --metrics=json
tsc --noEmit 2>&1 | axon filter tsc --budget=500 --metrics=json
journalctl -n 500 2>&1 | axon filter log --budget=700 --metrics=json
```

Escape para casos intencionais e pequenos:
```bash
AXON_ALLOW_RAW_SHELL=1 sed -n '1,80p' file.cpp
```

## Por que

Grep/Glob e shell bruto ruidoso são bloqueados por hook — contexto pré-indexado, filtros com orçamento, métricas e CCR eliminam buscas redundantes e outputs irreversíveis. Build-guard protege host de paralelismo agressivo. Write-through sincroniza filesystem ↔ índice sem ação manual do agente.
