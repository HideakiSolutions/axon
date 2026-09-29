# G10 Checkpoint — FalkorDB Graph Projection

- **Mini-goal:** repository-qualified graph projection and bounded traversal; accepted.
- **Facts observed:** the owner-authorized Shared Infrastructure offers FalkorDB at the documented
  local endpoint. `libhiredis` is optional at build time, so an Axon installation without it must
  retain all non-graph functionality.
- **Changes performed:** added the graph application port and an optional hiredis/FalkorDB adapter.
  Replacements stage a full generation, publish the stream state only after staging, then prune
  obsolete stream-local generations. Logical identities include `repository_id` and
  `index_stream_id`; physical identities additionally include generation. Neighbor relations retain
  direction, relation, distance and optional digest, with incoming edges reversed. Traversal is
  directed, generation/repository/stream filtered, ordered, bounded and explicitly truncated.
- **Tests executed:** `git diff --check`; real FalkorDB integration
  `test_portfolio_graph` passed 1/1; a clean configuration/build without hiredis passed for the
  main `axon` target.
- **Independent verifier:** accepted after rebuilding the current target and attempting to refute
  metadata fidelity, orientation, identity collision, generation publication/pruning, traversal
  bounds and isolation. No counterexample was found.
- **Risks/gaps:** graph read-model state is derived and can be rebuilt by stream. It deliberately
  exposes no arbitrary Cypher surface; the later bounded UI/API node owns graph presentation.
- **Functional percentage:** G10 implementation and its declared acceptance checks complete.
- **Rollback:** revert this isolated commit, or clear/rebuild the named derived FalkorDB graph.
- **Next action:** G11 multi-signal candidates, explanation and evaluation fixtures.
- **Human authority needed:** none for the local commit.
