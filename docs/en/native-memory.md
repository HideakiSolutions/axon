# Native memory operations

Axon keeps capture, retrieval, dialogue, and handoff state in the project's own `.axon/axon.duckdb`. There is no sidecar service and no automatic write to an external or canonical knowledge base.

## Recoverable write-through capture

Claude Code hooks append changed paths to `.axon/pending-writes.txt`. On the next MCP call, Axon atomically claims the batch as `.axon/pending-writes.processing`, deduplicates paths, and indexes them. The claim is acknowledged only after indexing succeeds.

- A crash leaves the processing claim available for replay.
- Failed attempts are counted in `.axon/pending-writes.attempts`.
- After five failed attempts, the batch is preserved as `.axon/pending-writes.failed-<epoch>.txt`; later batches can continue.
- Structured `pending_writes.*` events are emitted to stderr without entering the JSON-RPC stdout stream.

Failed batches are evidence, not dead-letter approval. Inspect and deliberately requeue their paths when the underlying indexing problem is fixed.

## Hybrid observation retrieval

`search_memory` retrieves semantic and lexical candidates, fuses their ranks with RRF (`k=60`), then multiplies by the saved observation's bounded `authority` value. Authority is clamped to 0.5–2.0 and changes ranking only; it never grants permission or approves a memory mutation.

Each result exposes the semantic and lexical ranks, lexical hit count, raw RRF score, authority, and final score. Exact identifiers can therefore complement semantic similarity without hiding why an item ranked.

Run the checked comparison fixture with:

```bash
AXON_EMBEDDING_MODEL=/path/to/model.gguf \
  python3 evals/run_memory_retrieval.py --axon build/axon
```

## Typed handoffs

Use `handoff_create` to persist a transfer rather than relying on free-form chat. Provide a target agent and objective; optionally bind it to a source session, a project-confined working directory, context, and an idempotency key.

Consumers should list or get the item, claim it atomically with `handoff_claim`, and finish it with `handoff_complete`. A retry by the same claimant returns the existing claim. A different claimant cannot steal it. `handoff_cancel` terminates pending or claimed work.

Session creation also accepts `idempotency_key`, scoped to its thread, so transport retries do not create duplicate sessions.

## Compatibility and approval boundary

Existing observations receive authority 1.0 through an additive database migration. Existing `session_start`, `save_observation`, and `search_memory` calls remain valid because all new inputs and output fields are additive.

Axon stores project-local operational memory. Promotion to a canonical vault or another governed memory system remains an explicit human-approved workflow outside these tools.
