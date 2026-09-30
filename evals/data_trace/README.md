# Rastreio de dados TS/JS: piloto experimental

`src/core/data_trace.hpp` expõe uma análise sob demanda de um arquivo TS/JS.
Ela acompanha definições e usos dentro de uma função, desde
`req`/`request` (`body`, `params`, `query`, `headers`) até argumentos de chamadas
estáticas de persistência ou HTTP. O resultado ordena origem, atribuições,
transformações e destino. O indexador normal não chama esta rotina, portanto o
recurso fica desativado por padrão.

O estado `confirmed` exige um caminho sem dependência condicional e um destino
reconhecido. O avaliador mescla valores depois de ramos; um caminho que depende
de um ramo, propriedades dinâmicas e retornos de chamadas auxiliares recebem
`unknown`.
Isso é uma aproximação conservadora de dependências de definição/uso, não um
CFG/PDG completo. Não cobre aliasing, closures, exceções, funções chamadas,
fontes fora de request nem métodos de persistência não listados.

`truth-v1.json` contém 12 exemplos rotulados: 3 elegíveis, 4 negativos e 5
desconhecidos. O avaliador `trace_eval.cpp` mede precisão apenas entre caminhos
confirmados, cobertura entre exemplos estáticos elegíveis e detecção dos casos
desconhecidos. Em 2026-09-30, compilado isoladamente com `-O1`, os resultados
foram 3/3 confirmados corretos, 0 falso positivo, 5/5 desconhecidos detectados,
614 µs para analisar 1.670 bytes de fonte e 5.513 bytes de rastros aproximados
em memória. O índice persistente mede 0 bytes porque esta implementação é
efêmera. São números apenas deste corpus pequeno; a expansão exige corpus
independente maior e medição em projetos reais.
