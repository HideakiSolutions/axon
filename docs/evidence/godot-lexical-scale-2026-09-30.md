# Escala da busca lexical local — 2026-09-30

O índice invertido `symbol_terms` substitui a leitura e retokenização de todos
os documentos lexicais a cada consulta. A consulta BM25 lê somente postings
dos termos pedidos. O indexador atualiza postings e comprimento junto com os
símbolos em substituições e remoções incrementais.

O ensaio reproduzível em
[`run_scale_probe.py`](../../evals/godot/run_scale_probe.py) copia o índice
Godot de 1.498 símbolos e duplica suas linhas dez vezes, chegando a 14.980
símbolos. Mede cinco invocações CLI completas e sem cache por modo em CPU. Os
dados brutos estão em
[`godot-lexical-scale-2026-09-30.json`](godot-lexical-scale-2026-09-30.json).

| Índice | Modo | Mediana | RSS máximo |
| --- | --- | ---: | ---: |
| Original | Semântico | 123,42 ms | 161,4 MiB |
| Original | Híbrido | 132,56 ms | 160,2 MiB |
| 10× | Semântico | 196,97 ms | 207,1 MiB |
| 10× | Híbrido | 217,25 ms | 207,2 MiB |

Antes da tabela invertida, uma sondagem exploratória no mesmo índice ampliado
mediu aproximadamente 518 ms por consulta híbrida e 263 MiB de RSS máximo.
Essa sondagem não é um benchmark pareado arquivado; a comparação reproduzível
acima é a referência para regressões futuras. As cópias sintéticas não incluem
arquivos fonte para os caminhos duplicados: este ensaio mede escala da consulta
ao índice, não qualidade da renderização desses corpos. O gate de qualidade e
orçamento da cápsula usa o corpus Godot original e está no documento vizinho.

Para reproduzir, compile `evals/godot/clone_index.cpp` contra a biblioteca
DuckDB do projeto e execute `run_scale_probe.py PROJECT --axon AXON
--clone-helper CLONE_HELPER --copies 10 --repeats 5` sobre uma cópia indexada
do runtime Godot. O script cria e apaga a cópia ampliada em um diretório
temporário; não altera o índice de origem.
