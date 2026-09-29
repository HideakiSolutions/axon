# Política de telemetria Axon

## Padrão operacional

Telemetria permanece **local e desativada por padrão**. `AXON_TELEMETRY=1` ou
`telemetry = true` requerem aprovação explícita do `project-owner` da raiz
registrada.

Quando autorizada, a coleta deve permanecer agregada (contagens, latência e
economia estimada). Não registrar prompts, conteúdo, paths de arquivos ou
segredos. Não configurar `AXON_TELEMETRY_ENDPOINT` sem uma decisão adicional
de exportação e retenção.

## Verificação

`axon metrics --json` deve reportar `telemetry_enabled: false` enquanto não
houver aprovação. A política não impede métricas locais já derivadas do índice;
ela impede a ativação de eventos de telemetria e qualquer exportação remota.

## Reversibilidade

Remover a variável de ambiente ou `telemetry = true` restaura o estado padrão.
