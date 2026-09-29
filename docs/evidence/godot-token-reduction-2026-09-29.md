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
