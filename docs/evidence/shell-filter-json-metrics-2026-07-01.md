# Shell Filter JSON Metrics Evidence - 2026-07-01

## Scope

- `axon filter <kind> [--budget=N] --metrics=json` emits machine-readable command metrics to stderr.
- `--json-metrics` is accepted as an alias.
- Default stderr remains the existing human-readable `[axon filter] ...` line.
- JSON metrics include command, output kind, budget, input/output tokens, tokens saved, changed flag, layer, latency, recoverability, and CCR artifact ID when present.

## Commands

```bash
cmake --build build --target axon -j2
bash tests/smoke/test_cli_filter_json_metrics.sh ./build/axon
rg -n "filter|compression|telemetry|artifact" src tests docs --glob '!build/**' > /tmp/axon-json-metrics-large-raw.txt
./build/axon filter grep --budget=500 --metrics=json < /tmp/axon-json-metrics-large-raw.txt > /tmp/axon-json-metrics-large-out.txt 2> /tmp/axon-json-metrics-large.err
jq -e '.type=="axon_filter_metrics" and .command=="grep" and .changed==true and .recoverable==true and (.ccr_artifact_id|startswith("ccr_")) and .output_tokens <= .budget and .tokens_saved > 0' /tmp/axon-json-metrics-large.err
artifact_id=$(sed -n 's/.*"ccr_artifact_id":"\([^"]*\)".*/\1/p' /tmp/axon-json-metrics-large.err)
./build/axon artifact-retrieve "$artifact_id" > /tmp/axon-json-metrics-large-recovered.txt
cmp -s /tmp/axon-json-metrics-large-raw.txt /tmp/axon-json-metrics-large-recovered.txt
./build/axon filter grep --budget=80 --json-metrics < /tmp/axon-json-metrics-large-raw.txt > /tmp/axon-json-metrics-alias-out.txt 2> /tmp/axon-json-metrics-alias.err
jq -e '.type=="axon_filter_metrics"' /tmp/axon-json-metrics-alias.err
./build/axon filter grep --budget=500 < /tmp/axon-json-metrics-large-raw.txt > /tmp/axon-human-metrics-large-out.txt 2> /tmp/axon-human-metrics-large.err
```

## Results

Representative JSON stderr:

```json
{"budget":500,"ccr_artifact_id":"ccr_18b272e09b28ddf559795d2206c9e10c113455f36d72cb7b7e795aca1f7dcd3b","changed":true,"command":"grep","input_tokens":16176,"kind":"plain_text","latency_ms":67,"layer":"shell_filtering","output_tokens":500,"recoverable":true,"tokens_saved":15676,"type":"axon_filter_metrics"}
```

Default stderr remained human-readable:

```text
[axon filter] kind=plain_text input_tokens=16176 output_tokens=500 saved=15676 changed=true ccr_artifact_id=ccr_18b272e09b28ddf559795d2206c9e10c113455f36d72cb7b7e795aca1f7dcd3b
```

## Fidelity Checks

- JSON parsed successfully and exposed numeric token fields.
- JSON reported `layer="shell_filtering"` for per-layer accounting.
- JSON reported `recoverable=true` and a `ccr_...` artifact ID for lossy output.
- `artifact-retrieve` returned the original raw grep trace byte-for-byte.
- `test_cli_filter_json_metrics` covers JSON metrics, the alias, default human stderr, and CCR retrieval in CTest.
