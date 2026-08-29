# Memory retrieval evaluation

This fixture compares the semantic-only order with Axon's final hybrid order on the same observations. The hybrid path uses reciprocal-rank fusion (RRF) across semantic and lexical candidates, then applies the bounded observation authority multiplier.

Run it with the same local GGUF model used by Axon:

```bash
AXON_EMBEDDING_MODEL=/path/to/nomic-embed-text-v1.5.Q4_K_M.gguf \
  python3 evals/run_memory_retrieval.py --axon build/axon
```

The command is deterministic apart from model/runtime differences, writes only to a temporary project, prints Recall@K, MRR and latency, and fails if hybrid Recall@K or MRR regresses below semantic-only, or if hybrid Recall@K falls below 100% on the checked fixture. The extreme authority clamp is tested separately by the MCP smoke test so it cannot distort this quality comparison.
