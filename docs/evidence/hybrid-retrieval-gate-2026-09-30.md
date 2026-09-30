# Gate de busca híbrida: Godot

O conjunto fixado em `evals/godot/retrieval-v1.json` contém quatro perguntas
originais em português, 16 perguntas amplas novas e oito consultas exatas. O
oráculo verifica marcadores de código na cápsula; a revisão humana conferiu se
os corpos bastam para responder. A avaliação usou a cópia de 78 scripts do
benchmark de 2026-09-29, índice reconstruído e embeddings em CPU, cache
desativado e orçamento de 8.000 tokens. O executor está em
`evals/godot/run_retrieval.py`; os resultados por pergunta estão em
[`godot-hybrid-retrieval-2026-09-30.json`](godot-hybrid-retrieval-2026-09-30.json).
Cada latência mede uma invocação CLI completa, incluindo início do processo e
carregamento do modelo; os dois modos foram medidos da mesma maneira.
O procedimento para recriar a cópia do checkout `3752a36` e o hash do modelo
estão no [benchmark original](godot-token-reduction-2026-09-29.md#reproduce).

| Medida final | Semântica | Híbrida | Limite |
| --- | ---: | ---: | ---: |
| Evidência automática | 7/28 (25%) | 28/28 (100%) | ≥80% |
| Perguntas originais com marcadores | 0/4 | 4/4 | ≥3/4 |
| Consultas exatas com evidência | 6/8 | 8/8 | Sem regressão |
| Cápsula máxima | 2.276 tokens | 3.447 tokens | ≤8.000 |
| Latência p95, CPU, sem cache | 134,05 ms | 141,37 ms (1,05×) | ≤1,5× |

**Gate aprovado.** `hybrid` passa a ser o padrão em MCP, CLI e HTTP;
`semantic` continua selecionável. O modo integra a chave de cache e a saída
expõe os ranks semântico e lexical por símbolo. Índices antigos com termos
persistidos são migrados para a tabela invertida ao abrir; índices anteriores
à extração lexical precisam de `axon index --force` para popular os documentos.

`save_pt` agora inclui `DialogueSession._persist_state()` chamando
`SaveLocal.save()`, `DialogueSession._load_state()` chamando `SaveLocal.load()`,
e os dois corpos de `SaveLocal`. A revisão humana confirma esse caminho de
salvamento e restauração. O oráculo mede presença de trechos e não mede a
qualidade de uma resposta gerada. Nas outras perguntas originais, trechos
secundários de câmera e escolha de diálogo ainda são truncados; portanto
4/4 significa suporte pelo critério fixado, não cobertura de todo o fluxo de
ponta a ponta. `movement_facing` e `dialogue_persist` tiveram seus corpos
principais verificados manualmente depois da expansão de chamadas locais.

Os documentos lexicais e seus postings são atualizados com cada arquivo
modificado e excluídos na remoção. Uma reabertura também repara comprimentos
ausentes de uma migração interrompida. Os testes focados de substituição,
exclusão, reconstrução e retomada passaram. Os três testes de orçamento da
cápsula também passaram.
`evals/godot/check_interfaces.py` comparou cápsulas sem cache entre CLI, MCP e
HTTP em `hybrid` e `semantic`, com conteúdo, seleção e orçamento idênticos.
Este gate cobre o corpus Godot medido; não demonstra
qualidade em todos os repositórios ou em GPU. Uma medição em GPU deve ser
registrada separadamente porque o benchmark inicial mostrou variação de rank
por dispositivo.
