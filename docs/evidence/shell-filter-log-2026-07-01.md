# Shell Filter Log Evidence - 2026-07-01

## Scope

- `axon filter log --budget=N` now uses a native log summarizer instead of only the generic log compressor.
- The filter counts detected levels, deduplicates repeated normalized messages, keeps distinct fatal/error/warn lines, and preserves first/last edge lines.
- Mixed malformed input passes through unchanged when most non-empty lines do not look like logs.
- Lossy CLI output remains CCR-recoverable through the emitted `axon:ccr` marker and `ccr_artifact_id`.
- The native log filter was compared against RTK on a real local `journalctl` trace.

## Commands

```bash
cmake --build build --target test_shell_filter axon -j2
./build/tests/test_shell_filter
journalctl -n 200 --no-pager > /tmp/axon-journal.log 2> /tmp/axon-journal.err || true
./build/axon filter log --budget=700 < /tmp/axon-journal.log > /tmp/axon-log-filtered.txt 2> /tmp/axon-log-filtered.err
rtk log /tmp/axon-journal.log > /tmp/axon-log-rtk.txt 2> /tmp/axon-log-rtk.err || true
artifact_id=$(sed -n 's/.*ccr_artifact_id=\([^ ]*\).*/\1/p' /tmp/axon-log-filtered.err)
./build/axon artifact-retrieve "$artifact_id" > /tmp/axon-log-recovered.txt
cmp -s /tmp/axon-journal.log /tmp/axon-log-recovered.txt
/usr/bin/time -f 'axon_elapsed=%e' ./build/axon filter log --budget=700 < /tmp/axon-journal.log > /tmp/axon-log-filtered.txt
/usr/bin/time -f 'rtk_elapsed=%e' rtk log /tmp/axon-journal.log > /tmp/axon-log-rtk.txt
```

Token estimates use Axon's current `(bytes + 3) / 4` approximation.

## Results

| Case | Bytes | Tokens | Latency | Notes |
| --- | ---: | ---: | ---: | --- |
| Raw `journalctl -n 200` trace | 20,140 | 5,035 | n/a | 200 log lines |
| `axon filter log --budget=700` | 1,712 | 428 | 0.09s | Saved 4,607 tokens, includes CCR marker |
| `rtk log /tmp/axon-journal.log` | 102 | 26 | 0.01s | Produced an empty-level summary for this trace |
| `axon artifact-retrieve <id>` | 20,140 | 5,035 | n/a | Byte-for-byte match with original stdin |

Axon stderr:

```text
[axon filter] kind=log input_tokens=5035 output_tokens=428 saved=4607 changed=true ccr_artifact_id=ccr_8ad961e7e28799482fd30e176ff7f637458bcf6d7fb7956844d258af3956d5e7
```

Axon retained the main signal:

```text
# axon log summary: 200 log lines, 182 signal lines
levels: error=6 info=176 other=18

## repeated messages
97x [info] Started app-com.google.Chrome-#.scope.
6x [error] can't connect to the daemon /usr/lib/gnupg/scdaemon: IPC connect call failed

## important
[error] Jun 29 23:53:39 dev-claude gpg-agent[1626799]: can't connect to the daemon /usr/lib/gnupg/scdaemon: IPC connect call failed
```

RTK output for the same trace:

```text
Log Summary
   [error] 0 errors (0 unique)
   [warn] 0 warnings (0 unique)
   [info] 0 info messages
```

## Fidelity Checks

- Unit tests verify level counts, repeated-message deduplication, important fatal/error/warn retention, first/last edge retention, alias normalization, and tight budget compliance.
- Unit tests verify malformed mostly-non-log input passes through unchanged.
- End-to-end CLI smoke verified the lossy output stayed below `--budget=700` after CCR marker overhead.
- End-to-end CLI smoke verified `artifact-retrieve` returns the exact original journal trace.

## Remaining Gaps

- Shell-filter benchmark evidence now covers diff, grep, JSON, TypeScript compiler, test output, package-manager output, lint output, log output, and shell-filter CCR recovery.
- Machine-readable shell-filter metrics were added later on 2026-07-01; see `docs/evidence/shell-filter-json-metrics-2026-07-01.md`.
- A final requirement-by-requirement completion audit should confirm whether any remaining critical shell command family or documentation claim still depends on unvalidated behavior.
