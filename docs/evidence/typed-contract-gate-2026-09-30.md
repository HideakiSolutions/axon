# Typed contract pilot — 2026-09-30

The production extractor indexes declared OpenAPI operation IDs, AsyncAPI 3
single-server/single-channel operations, and `.proto` package/service/RPC
methods. It observes literal Express routes, literal Python/Go HTTP calls,
generated C# RPC clients, matching Java RPC providers, and static JS/Python/Go
topic sends/subscriptions in the pilot patterns. A resolved link requires a
qualified identity and producer/consumer evidence across repositories. Runtime
computed routes and topics, ambiguous RPC packages, and unresolved AsyncAPI
channels remain unknown.

The three-repository corpus contains TypeScript, JavaScript, Python, Java, Go,
and C#. Its labels include route/method mismatches, distinct services with the
same RPC method, homonymous topics on separate brokers, and dynamic calls. The
standalone evaluator uses `src/core/contracts.cpp`, counts every confirmed
link absent from the positive labels as an error, and requires at least five
positives and two negatives per surface. Results are in
[`typed-contracts-2026-09-30.json`](typed-contracts-2026-09-30.json).

| Surface | TP | FP | FN | TN | Unknown | Precision | Recall |
|---------|---:|---:|---:|---:|--------:|----------:|-------:|
| HTTP | 6 | 0 | 0 | 2 | 1 | 100% | 100% |
| OpenAPI | 6 | 0 | 0 | 2 | 0 | 100% | 100% |
| gRPC | 6 | 0 | 0 | 5 | 0 | 100% | 100% |
| Topic / AsyncAPI | 8 | 0 | 0 | 3 | 1 | 100% | 100% |

The corpus gate passed: 26 confirmed links, zero unlabelled links. A separate
MCP smoke created three local indexes and checked HTTP, OpenAPI, RPC and topic
links, dynamic unknown evidence, broker isolation, and the legacy candidate
field. The focused incremental test checked `.proto`/OpenAPI insertion,
replacement, deletion, rebuild, no-op event behavior and manifest changes;
all focused `PortfolioJournalTest` cases passed.

Reproduce the evaluator with the compile and run commands in
[`evals/contracts/README.md`](../../evals/contracts/README.md). For the MCP
smoke: `python3 evals/contracts/check_group_impact.py --axon /path/to/axon`.

This is a pilot corpus, not a claim of complete framework or language coverage.
The extractor deliberately leaves other dynamic and multi-server patterns
unknown. The retrieval gate passed earlier in this change, so `group_impact`
returns typed links as its primary result for the documented static patterns.
Coverage outside those patterns remains unknown and needs broader
repository-scale validation.
