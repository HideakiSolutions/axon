# Operação da memória nativa

O Axon mantém captura, recuperação, diálogo e handoffs no `.axon/axon.duckdb` do próprio projeto. Não existe serviço sidecar nem escrita automática em uma base de conhecimento externa ou canônica.

## Captura write-through recuperável

Os hooks do Claude Code acrescentam caminhos alterados em `.axon/pending-writes.txt`. Na chamada MCP seguinte, o Axon reivindica atomicamente o lote como `.axon/pending-writes.processing`, remove caminhos duplicados e os indexa. A reivindicação só é confirmada após a indexação concluir com sucesso.

- Uma interrupção mantém o lote em processamento disponível para replay.
- Tentativas com falha são contadas em `.axon/pending-writes.attempts`.
- Depois de cinco falhas, o lote é preservado como `.axon/pending-writes.failed-<epoch>.txt`; lotes posteriores podem prosseguir.
- Eventos estruturados `pending_writes.*` são emitidos em stderr, sem contaminar o stdout JSON-RPC.

Um lote falho é evidência, não aprovação automática. Inspecione-o e recoloque deliberadamente seus caminhos na fila depois de corrigir a causa da falha.

## Recuperação híbrida de observações

`search_memory` recupera candidatos semânticos e lexicais, funde seus ranks com RRF (`k=60`) e depois multiplica pela `authority` limitada da observação. A autoridade é restringida a 0,5–2,0 e altera apenas o ranking; nunca concede permissão nem aprova mutação de memória.

Cada resultado expõe ranks semântico e lexical, quantidade de acertos lexicais, score RRF bruto, autoridade e score final. Identificadores exatos complementam a similaridade semântica sem esconder o motivo do ranking.

Execute a avaliação comparativa versionada com:

```bash
AXON_EMBEDDING_MODEL=/caminho/para/model.gguf \
  python3 evals/run_memory_retrieval.py --axon build/axon
```

## Handoffs tipados

Use `handoff_create` para persistir uma transferência, em vez de depender apenas de texto livre. Informe agente de destino e objetivo; opcionalmente associe sessão de origem, diretório de trabalho confinado ao projeto, contexto e chave de idempotência.

O consumidor lista ou consulta o item, reivindica-o atomicamente com `handoff_claim` e finaliza com `handoff_complete`. Um retry pelo mesmo agente devolve a reivindicação existente; outro agente não pode tomá-la. `handoff_cancel` encerra trabalho pendente ou reivindicado.

A criação de sessão também aceita `idempotency_key`, limitada ao thread, evitando sessões duplicadas em retries de transporte.

## Compatibilidade e limite de aprovação

Observações existentes recebem autoridade 1,0 por migração aditiva. Chamadas existentes de `session_start`, `save_observation` e `search_memory` continuam válidas, pois todas as entradas e saídas novas são aditivas.

O Axon armazena memória operacional local ao projeto. A promoção para um vault canônico ou outro sistema governado continua sendo um fluxo explícito, com aprovação humana, fora dessas ferramentas.
