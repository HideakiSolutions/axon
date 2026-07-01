# Shell Filter CCR Evidence - 2026-07-01

## Scope

- Changed `axon filter` output is now recoverable by default.
- The CLI stores original stdin as a `shell_filter` CCR artifact before emitting a lossy shell summary.
- The emitted output is prefixed with an `axon:ccr` marker and stderr includes `ccr_artifact_id=<id>`.
- If artifact storage fails, or the marker overhead removes token savings or exceeds `--budget`, Axon passes through the original output instead of emitting an unrecoverable summary.

This note validates CLI shell-filter recovery. Capsule body CCR remains covered by `docs/evidence/ccr-artifact-retrieve-2026-06-30.md`.

## Commands

```bash
cmake --build build --target axon test_ccr -j2
rg -n "filter|compression|telemetry|artifact" . --glob '!third_party/**' --glob '!build/**' --glob '!editors/vscode/dist/**' --glob '!editors/vscode/node_modules/**' > /tmp/axon-shell-ccr-raw.txt
./build/axon filter grep --budget=700 < /tmp/axon-shell-ccr-raw.txt > /tmp/axon-shell-ccr-filtered.txt 2> /tmp/axon-shell-ccr.err
artifact_id=$(sed -n 's/.*ccr_artifact_id=\(ccr_[^ ]*\).*/\1/p' /tmp/axon-shell-ccr.err)
./build/axon artifact-retrieve "$artifact_id" > /tmp/axon-shell-ccr-retrieved.txt
cmp -s /tmp/axon-shell-ccr-raw.txt /tmp/axon-shell-ccr-retrieved.txt
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

| Payload | Tokens | Notes |
| --- | ---: | --- |
| Raw grep trace | 15,343 | Baseline stdin |
| `axon filter grep --budget=700` | 700 | Includes CCR marker and stayed within requested budget |
| `axon artifact-retrieve <id>` | 15,343 | Byte-for-byte match with original stdin |

Axon stderr:

```text
[axon filter] kind=plain_text input_tokens=15343 output_tokens=700 saved=14643 changed=true ccr_artifact_id=ccr_1014fe8772cb100e79f5b8049dedf43e4db6cb8bb54eea01dd5759db027d8f51
```

## Fidelity Checks

- Unit tests verify recoverable CCR output stores the original and prepends a marker.
- Unit tests verify marker overhead fallback returns the original output unchanged.
- End-to-end CLI smoke verified `artifact-retrieve` returns the exact original shell trace.
- End-to-end CLI smoke verified final output stayed within `--budget=700` after marker overhead.

## Remaining Gaps

- Native command-family RTK comparisons now include diff, grep, JSON, TypeScript compiler, test output, package-manager output, lint output, and log output.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
