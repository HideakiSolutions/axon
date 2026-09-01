# Portfolio candidate truth set v1

This versioned, source-free fixture names the six deterministic classification boundaries used by
`test_capability_candidates`. The executable runner invokes the compiled
`CapabilityCandidateGenerator` for **every case**, rather than validating the fixture alone:

```sh
python3 evals/portfolio/run_portfolio_eval.py --evaluator /path/to/portfolio_eval
```

Its JSON report contains, for each name-only, semantic-only and multi-signal baseline, the real
`tp`, `fp`, `fn`, `precision_at_1`, `recall_at_1`, plus the complete classification false-positive
and false-negative lists. Here a true positive means the engine's top classification exactly equals
the labelled classification; a mismatch is recorded as both the incorrect predicted class (FP) and
the missed expected class (FN). The runner is deterministic, source-free, and uses no network or
embedding service.

The separate three-repository DuckDB integration corpus verifies read-only projection, package
consumption metadata, observed implementations, and declaration drift. Its classification coverage
is connected to this truth-set runner through the same `CapabilityCandidateGenerator` contract.

Run the reproducible local performance baseline after building the E2E target:

```sh
scripts/benchmark_portfolio.sh /tmp/axon-g12-build
```

It measures wall-clock duration for the real three-repository catalog journey and the compiled
candidate evaluator. Results are environment-specific and must not be interpreted as a cross-host
performance guarantee.

The fixture contains metadata labels only. It neither assigns ownership nor authorizes a package,
component extraction or refactor.
