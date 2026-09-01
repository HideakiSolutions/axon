# G11 Checkpoint — Multi-Signal Candidates and Explanations

- **Mini-goal:** deterministic, bounded candidate generation and classification; accepted after
  owner-authorized correction beyond budget 2/2.
- **Facts observed:** capability signatures already contain metadata-only contract, endpoint, event,
  AST, dependency, graph-neighborhood, test and bounded-context channels. Semantic retrieval is
  optional, so its result is accepted only as a bounded validated hint.
- **Changes performed:** added a provider-independent multi-signal engine with weighted RRF,
  deterministic tie breaking, endpoint fan-out caps, all six classifications, recommendations,
  freshness, confidence, score/rank/evidence fields, matching references, differences and
  invalidators. Candidate promotion requires independent evidence; same-name/different-domain and
  name-only/semantic-only cases remain non-promoting. Every deterministic material channel has a
  corresponding difference explanation.
- **Tests executed:** `test_capability_candidates` passed 11/11; `git diff --check` passed. The
  executable seed evaluates P@1/R@1 as 0/0 for name-only and semantic-only baselines and 1/1 for
  the multi-signal engine. The versioned truth set validates all required classifications.
- **Independent verifier:** initially found three real defects: exact duplicate ignored behavioral
  route/event differences; endpoint caps did not constrain final fan-out; handler differences were
  not reported. Each was corrected. The final technical re-refutation accepted all fixes and found
  no remaining counterexample.
- **Risks/gaps:** G15 owns the larger three-repository corpus and full precision/recall report; the
  G11 seed is deliberately bounded and source-free. Candidates only recommend review and make no
  ownership, package or code-change mutation.
- **Functional percentage:** G11 implementation and declared acceptance checks complete.
- **Rollback:** revert this isolated commit; derived candidate results can be regenerated.
- **Next action:** G12 read-only Git capability-graph declarations and observed/declaration drift.
- **Human authority needed:** the responsible owner authorized this third G11 correction on
  2026-09-01; no further authority is needed for this local commit.
