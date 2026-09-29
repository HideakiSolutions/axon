# Axon como camada primaria e RTK opcional

Use este guia quando o Axon for a camada local primaria de contexto para agentes e o RTK ficar apenas como fallback opcional.

## Setup alvo

```json
{
  "mcpServers": {
    "axon": {
      "command": "/caminho/para/axon/build/axon",
      "args": ["serve"]
    }
  }
}
```

Indexe o projeto antes da sessao do agente:

```bash
axon init .
axon index --force
```

Para capsulas mais ricas, habilite granularidade simbolica e reindexe:

```toml
granularity = "symbol"
token_budget = 8000
capsule_compression = "body"
```

## Regras de roteamento do agente

Use Axon antes de leituras brutas:

| Tarefa | Caminho primario no Axon |
| --- | --- |
| Entender um repo | `get_overview` depois `get_context_capsule` |
| Trabalhar numa mudanca focada | `get_context_capsule` com query e `token_budget` explicito |
| Expandir arquivo de capsula | Use o `expand_command` retornado |
| Inspecionar assinaturas | `get_skeleton` |
| Rastrear impacto | `get_impact_graph`, `get_callers`, `get_tests_for` |
| Recuperar conteudo lossy | `artifact_retrieve` ou `axon artifact-retrieve <id>` |

Entradas de arquivo em capsulas incluem `source_ref` e `expand_command`. Trate esses campos como o primeiro caminho de expansao. Leituras brutas de arquivo sao fallback apenas quando a expansao via Axon for insuficiente.

## Filtros de output shell

Passe outputs grandes pelos filtros do Axon:

```bash
some-command 2>&1 | axon filter auto --budget=800 --metrics=json
rg -n "symbol" src | axon filter grep --budget=600 --metrics=json
npm test 2>&1 | axon filter test --budget=700 --metrics=json
```

Projetos instalados no Claude Code incluem `axon-shell-guard.sh`, um hook Bash `PreToolUse` que nega comandos brutos ruidosos em repos indexados, exceto quando roteados por `axon filter`, fallback compativel RTK, redirecionamento de stdout ou escape explicito `AXON_ALLOW_RAW_SHELL=1`.

Familias nativas:

| Familia | Aliases |
| --- | --- |
| `diff` | `git-diff` |
| `grep` | `rg` |
| `json` | `json` |
| `tsc` | `typescript`, `compiler` |
| `test` | `pytest`, `vitest`, `ctest`, `gtest` |
| `package` | `npm`, `pnpm`, `yarn`, `bun` |
| `lint` | `eslint`, `ruff`, `prettier`, `format` |
| `log` | `logs` |

Output lossy alterado e recuperavel por padrao. O resumo emitido contem marcador `axon:ccr` e as metricas JSON incluem `ccr_artifact_id` quando `--metrics=json` e usado.

## Politica de fallback para RTK

Mantenha RTK instalado apenas para compatibilidade ou conferencia:

- Use RTK quando uma familia de comando ainda nao existir em `axon filter`.
- Use RTK para comparar qualidade de reducao durante benchmarks.
- Nao roteie contexto normal via RTK quando `get_context_capsule`, `get_skeleton` ou `expand_command` puderem responder primeiro.

Rode a comparacao agregada antes de release ou apos mudar filtros shell:

```bash
bash scripts/benchmark_shell_filters.sh /tmp/axon-shell-filter-bench
```

O runner valida budget, economia de tokens e recuperacao CCR para toda familia nativa, e registra tokens de saida do RTK quando RTK esta disponivel.

## Gate de release

Antes de tratar Axon como camada primaria em release:

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
bash scripts/benchmark_shell_filters.sh /tmp/axon-shell-filter-bench
```

A suite CTest inclui smoke tests para metricas JSON de shell, schema MCP de capsula, freshness de docs e benchmark shell agregado quando as dependencias locais estao disponiveis.
