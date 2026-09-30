# Godot / GDScript token reduction on porto-godot-lan

Date: 2026-09-29. Tested Axon's GDScript integration against a copy
of `/workspace/local/porto-godot-lan/src` at Git HEAD `3752a36`. The source
checkout was not modified. The benchmark copy and raw results are under
`/tmp/axon-porto-bench-iNP7OM`.
Durable per-file measurements are in
[`godot-skeleton-measurements-2026-09-29.csv`](godot-skeleton-measurements-2026-09-29.csv),
and capsule selection and estimates are in
[`godot-capsule-measurements-2026-09-29.json`](godot-capsule-measurements-2026-09-29.json).

## Method

- Count `.gd` source bytes and skeleton output bytes, then estimate tokens as
  `ceil(bytes / 4)` per file, matching Axon's approximate token accounting.
- Index the eligible full corpus without embeddings, and generate one skeleton
  for every indexed script.
- Index 78 game/runtime scripts with `nomic-embed-text-v1.5.Q4_K_M.gguf`, then
  run uncached default-budget (`8,000`) context capsules for three broad queries.
- Compare capsule token estimates with the same raw-source estimator for the
  selected files. Inspect MCP capsule contents for targeted queries.

This is an estimate of context size, **not** measured billing or end-to-end LLM
token use. Retrieval relevance was checked separately; a short capsule does
not establish that it contains the code needed to answer a question.

## Results

| Mode | Corpus | Raw estimated tokens | Output estimated tokens | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Skeletons | 252 scripts | 562,516 | 69,756 | 87.60% |
| Skeletons | 78 runtime scripts | 190,165 | 20,868 | 89.03% |
| Capsule: `player movement camera rig world input` | 6 selected scripts | 30,503 | 539 | 98.23% |
| Capsule: `dialogue choice state consequence` | 7 selected scripts | 47,232 | 2,511 | 94.68% |
| Capsule: `mission state save load persistence` | 7 selected scripts | 22,486 | 1,007 | 95.52% |

The full structural index completed with 252 files, 5,245 symbols, and 199
edges. All 252 skeleton requests succeeded; no output was empty. The runtime
index completed with 78 files, 1,498 symbols, 18 edges, and 1,498 embeddings.

There are 269 `.gd` files in the source checkout. Axon's generic `build/`
directory exclusion skipped 17 scripts under `tools/build/` (130,685 bytes).
The eligible corpus contained 2,249,677 source bytes; skeleton output was
278,657 bytes. The runtime subset contained 760,530 source bytes; skeleton
output was 83,356 bytes.

## Relevance and parser findings

The broad movement capsule omitted `src/entities/player/player.gd` and
`src/entities/player/camera_rig.gd`, despite the topic. A targeted
`compute_velocity` query returned the function body in `player.gd` (406
estimated tokens for that file). A query containing `PlayerCharacter
compute_velocity movement` selected `player.gd` but returned only its class
declaration (28 estimated tokens for that file). The smaller result was less
useful despite its larger nominal reduction. Exact symbol queries selected
the expected file in four of four checks, but file selection alone is an
insufficient relevance test. The two MCP payload checks are recorded as
content hashes, headers, and signature-presence flags in
[`godot-mcp-retrieval-2026-09-29.json`](godot-mcp-retrieval-2026-09-29.json).

The first full-corpus index exposed a crash on the unnamed enum in
`addons/gut/diff_tool.gd`. The GDScript parser now handles a null Tree-sitter
`name` field, and the parser smoke test includes an unnamed enum. The
one-file reproducer and full eligible corpus indexed successfully afterward.

## Reproduce

Create independent copies of the 269 GDScript files and two `project.godot`
files. The runtime set is exactly the recursive `.gd` files below the seven
top-level directories shown here:

```sh
source_root=/workspace/local/porto-godot-lan/src
bench_root=$(mktemp -d)
mkdir -p "$bench_root/full" "$bench_root/runtime"
(cd "$source_root" && find . -type f \( -name '*.gd' -o -name 'project.godot' \) -print0 |
  tar --null -T - -cf -) | (cd "$bench_root/full" && tar -xf -)
for dir in src world ui dialogue persistence domain content; do
  (cd "$source_root" && find "$dir" -type f -name '*.gd' -print0 |
    tar --null -T - -cf -) | (cd "$bench_root/runtime" && tar -xf -)
done
cp "$source_root/project.godot" "$bench_root/runtime/project.godot"
```

With a built Axon binary, index the copies separately. The full-corpus run
used no available embedding model; the runtime run used the model below.
Run `axon skeleton` for each of the 252 index-eligible `.gd` files in the full
copy, and sum `ceil(source_bytes / 4)` and `ceil(output_bytes / 4)` per file.
The broad capsule queries were:

```sh
axon index "$bench_root/full"
export AXON_EMBEDDING_MODEL=/path/to/nomic-embed-text-v1.5.Q4_K_M.gguf
cd "$bench_root/runtime"
axon index .
axon capsule 'player movement camera rig world input' --no-cache
axon capsule 'dialogue choice state consequence' --no-cache
axon capsule 'mission state save load persistence' --no-cache
axon capsule 'compute_velocity' --no-cache
```

The model used for this run was the [nomic-ai GGUF Q4_K_M release](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/blob/main/nomic-embed-text-v1.5.Q4_K_M.gguf), SHA-256
`d4e388894e09cf3816e8b0896d81d265b55e7a9fff9ab03fe8bf4ef5e11295ac`.

## Answer support check (2026-09-29)

The same 78-file index was queried through the MCP `get_context_capsule` tool
with `no_cache=true` and an 8,000-token budget. Each percentage below compares
the capsule's estimated tokens with `ceil(bytes/4)` for the **union of its
selected pivot and support files**, not with the whole repository. The assessment
asks whether the returned text alone supports a correct answer; the source files
were then read to establish the reference behavior. It is a retrieval quality
check, not an end-to-end language-model or billing measurement.

| Question or search | Capsule / selected raw | Reduction | Answer support |
| --- | ---: | ---: | --- |
| Portuguese: how movement velocity is computed | 425 / 20,455 | 97.92% | Insufficient: `player.gd` contributes only an unrelated constant. |
| Exact `compute_velocity` | 581 / 23,224 | 97.50% | Sufficient: the function body exposes camera-relative direction, diagonal normalization, acceleration, gravity, and floor handling. |
| Portuguese: camera following and zoom | 695 / 19,130 | 96.37% | Insufficient: `camera_rig.gd` is absent. |
| English: camera rig follow/zoom | 1,678 / 49,995 | 96.64% | Partial: `camera_rig.gd` is selected, but only `_zoom_progress` is shown. |
| Exact `_advance_follow_state` | 2,029 / 50,778 | 96.00% | Partial: follow and anticipation appear, but the body is truncated before the manual-return behavior. |
| Exact `_apply_zoom_and_overview` | 1,296 / 50,522 | 97.43% | Partial: distance and elevation appear, but the body is truncated. |
| Portuguese: dialogue choices and consequences | 443 / 13,705 | 96.77% | Insufficient: no dialogue session or state logic is returned. |
| English: dialogue choice/state/consequence | 2,511 / 47,232 | 94.68% | Partial: related files appear, but the commit/effect implementation does not. |
| Exact `_apply_pending_option` | 1,124 / 52,677 | 97.87% | Sufficient for the effect: `collect` calls mission rules and persists only on a changed state. |
| Portuguese: save and restore mission state | 331 / 15,525 | 97.87% | Insufficient: `save_local.gd` is absent. |
| English: mission state save/load | 1,007 / 22,486 | 95.52% | Partial: the start of `save` appears, truncated before the write; `load` is absent. |
| Exact `SaveLocal load` | 578 / 26,318 | 97.80% | Insufficient: only the `SaveLocal` class declaration appears. |

The reference source confirms that `SaveLocal.load()` distinguishes empty and
corrupt records, parses through notebook rules, and updates the last-written
state only after validation. None of the tested save capsules supports that
answer. Likewise, a correct dialogue answer needs `choose_option()` and
`_apply_pending_option()` together; the broad capsules do not provide both.
Across the four broad Portuguese questions, **zero** capsules contain enough
code for a complete answer. Exact symbol searches improve movement and
dialogue-effect coverage, but body truncation and missed symbol selection
remain material limits. The compact context therefore cannot be treated as an
equivalent replacement for source reading on these questions.

A guided follow-up with `query="load"` and
`pivot_files=["persistence/save_local.gd"]` did return the complete 11,428-byte
file (2,857 estimated tokens), including `load()` and its validation branches.
This repairs answer support by expanding the source; it provides no reduction
for that file. A realistic workflow must spend additional tool calls and
context when the first capsule lacks the needed evidence.

The installed v1.4.0 Linux package was also exercised against this index. Its
embedding path terminated with `SIGILL` on an Intel i7-8700K (AVX2, no AVX-512)
before a capsule was returned. The quality check above used a local source
build on the same host. The release-build compatibility defect is addressed in
v1.5.0 by disabling ggml's host-native instruction selection for x64 packages;
the final packaged binary must pass a model-loading smoke on this host before
installation is considered verified.

### CPU/GPU preference check

The follow-up Vulkan build was exercised on the same i7-8700K host with an
NVIDIA GeForce RTX 4060. `AXON_EMBEDDING_DEVICE=cpu` logged `CPU`;
`gpu` and the default `auto` logged `Vulkan0` after the backend identified the
RTX 4060. All three returned a 581-token capsule for `compute_velocity`.
An invalid preference failed with an explicit validation error. A separate
one-file Godot fixture indexed and embedded two symbols on `Vulkan0`.

The 13 uncached MCP questions above were then replayed against the same
CPU-built index using GPU inference. Nine retained the same pivot order and
token estimate; four changed selected pivots or output size (`camera_en`,
`dialogue_en`, `save_pt`, `dialogue_choice_exact`). The answer-support verdicts
for those four did not worsen: the broad camera/dialogue/save questions were
still partial or insufficient, while the exact dialogue choice still returned
its function. Backend floating-point differences can therefore change ranking
near a selection boundary; token reduction and answer quality should be
evaluated per device rather than assumed bit-identical.

The complete 78-file runtime subset was also re-indexed on `Vulkan0`: 1,498
symbols and 18 edges, with all 1,498 symbols embedded. Replaying the 13 queries
against this GPU-built index retained the same pivot order and capsule size
for five queries; eight differed. The four broad Portuguese questions remained
insufficient, and the exact movement and dialogue-effect queries remained
sufficient for their respective questions. Per-query selections and token
estimates for CPU inference/index, GPU inference on the CPU index, and GPU
inference/index are recorded in
[`godot-device-parity-2026-09-29.json`](godot-device-parity-2026-09-29.json).
