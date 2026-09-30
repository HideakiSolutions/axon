# Consultas de grafo e rastreio de dados: pilotos de 2026-09-30

As etapas de busca híbrida e contratos tipados passaram seus gates medidos nos
corpora registrados nos documentos vizinhos. As consultas abaixo são expostas
como pilotos explícitos e ainda não são uma garantia geral para qualquer
framework ou linguagem.

## Formato de API

`api_shape` compara somente um subconjunto estático: provedores HTTP vinculados
por contrato e respostas TS/JS `res.json({...})` literais contra acessos
`response.data.campo` literais. Um campo ausente só é incompatibilidade
**comprovada** se todas as respostas reconhecidas têm forma fechada. Spread,
resposta dinâmica, retorno alternativo e falta de vínculo produzem resultado
possível ou desconhecido. O teste rotulado `test_impact_query_adapter` cobre
duas respostas com interseção de campos, spread/resposta dinâmica e vínculo
ausente. `producer_origin` e `consumer_origin` acompanham cada achado.

## Comunidades e execução

`symbol_communities` agrupa componentes conexas de chamadas/imports do índice
com ordenação determinística. `execution_flow` começa apenas em entradas HTTP,
RPC ou eventos com identidade de contrato resolvida, percorre no máximo oito
arestas e cinquenta caminhos por padrão e devolve `truncated` quando atinge
limites. Cada aresta traz origem da evidência e indicação de incerteza. As
arestas de chamadas e imports existentes são incertas: a resolução de alvos do
índice é heurística. Saídas de persistência, HTTP externo, publicação e retorno
são detectadas por padrões estáticos e marcadas como inferidas. Testes
rotulados cobrem entradas das três superfícies, ordenação, ciclos e limites.

## Rastreio de dados

`trace_data_flow` / `axon data-trace` é chamado sob demanda para TS/JS. O
indexador não o executa, portanto o índice persistente adicional é zero.
O corpus `evals/data_trace/truth-v1.json` tem doze casos: três elegíveis,
quatro negativos e cinco desconhecidos. A avaliação isolada em CPU obteve
precisão 3/3, cobertura elegível 3/3, negativos 4/4 e desconhecidos 5/5;
1.670 bytes de fonte foram analisados em 614 µs, com cerca de 5.513 bytes
de rastros em memória. Esses números são do pequeno corpus sintético.
O [recibo JSON](data-trace-2026-09-30.json) registra esta execução; a duração
varia entre execuções e não é um limite de desempenho estabelecido.
O algoritmo segue definições/usos dentro da função e mescla estados após
ramos conhecidos; caminhos condicionais para um destino, propriedades dinâmicas
e chamadas auxiliares ficam como desconhecidos.
Não há CFG/PDG completo, fluxo entre funções ou cobertura medida em projetos reais.

Interfaces CLI e MCP compartilham a mesma função de consulta e a mesma saída
JSON. A ausência de achados significa ausência de evidência avaliada, não
ausência de consumidores.

Verificação integrada: os três testes `test_impact_queries`,
`test_impact_query_adapter` e `test_data_trace` passaram. O script
`python3 evals/contracts/check_impact_interfaces.py --axon /path/to/axon`
criou dois índices reais e comparou CLI/MCP para as quatro consultas; também
verificou uma incompatibilidade comprovada de campo HTTP e um rastreio
confirmado. O smoke de contratos com três índices e os 20 testes de journal
passaram após a integração.
