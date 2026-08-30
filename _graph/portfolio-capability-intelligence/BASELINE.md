# Portfolio Capability Intelligence Baseline

## Identity

- Goal: production-ready federated Portfolio Capability Intelligence.
- Source baseline: `main` at `f776d73a95628e14328ed4b9f66d89318c437832`.
- Feature branch: `feature/portfolio-capability-intelligence`.
- Worktree: `/opt/hideakisolutions/.worktrees/axon-portfolio-capability-intelligence`.
- Repository id: `7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb`.
- Immutable tag: `v1.2.16` -> `aefa8e95aa9422468aa69bd431918d4069c5157f`.

## Authority and Scope

- Authority: user authorized continued implementation; merge, release, deployment, secrets,
  infrastructure mutation, database migration and external capability-graph writes remain gated.
- Architectural status: ADR-0003 is Proposed and requires explicit owner acceptance before G2.
- Capability status: no exact platform graph node exists; promotion intake is Proposed and requires
  owner/platform disposition before transversal production contracts are implemented.
- Rollback: remove unmerged worktree/branch or revert one isolated node commit; derived projections
  are disposable.
- Stop conditions: governance conflict, destructive/incompatible change, two failed corrections,
  unexplained regression or missing external authority.

## Observed Facts

- Main checkout has preexisting untracked `.claude/` and `_graph/release-v1.2.16/`; they were not
  copied or modified.
- The consolidated study was created by this goal and copied into the isolated worktree.
- Axon contains project-local DuckDB, registry/groups, live multi-repo HTTP aggregation, RRF memory
  precedent and additive MCP/HTTP surfaces.
- Axon lacks a schema ledger, transactional index journal, tombstones, durable portfolio projection,
  capability signatures and declared/observed drift.
- Existing `group_impact` opens secondaries inconsistently, references obsolete `edges.from_id` /
  `to_id` columns and swallows failures.
- Shared `shared-postgres` (`pgvector/pgvector:pg16`), `shared-qdrant` (v1.12.4) and
  `shared-falkordb` were running healthy on 2026-08-30.
- Shared-infrastructure policy requires consuming those services rather than declaring duplicates.
- Initial platform-registry discovery used broad adjacent terms and found capability-intake,
  graph and provider capabilities. This is baseline discovery only, not the exact-query evidence
  required to validate the proposed intake; exact capability, contract, module and owner queries
  remain a G1 obligation. The validator also reported two preexisting manifest drifts in
  `ui.login`/legacy component schema.
- The mandated pinned corpus validator is missing from the current enterprise-hseos checkout.
- Axon-first discovery was blocked by a preserved live v1.2.16 DuckDB owner; direct source/test/docs
  inspection was the declared fallback.

## Baseline Verification

- Initial configure failed only because worktree submodules were not initialized.
- `git submodule update --init --recursive` checked out all commits pinned by the baseline.
- Release configure passed with GCC 13.3.0.
- Release build passed with `cmake --build build -j2`.
- One preexisting warning: unused `sq2` lambda in `src/core/embeddings.cpp:121`.
- CMake baseline declares 3.20 and does not set `CMAKE_CXX_EXTENSIONS OFF`, while the current C++
  standard requires CMake 3.28+ and extensions disabled; this is a recorded preexisting compliance
  gap for G15, not silently attributed to the feature.
- No repository OpenAPI/Pact/coverage/clang-tidy configuration was found in the baseline; new public
  portfolio contracts require explicit artifacts and G15 must either close or govern applicable
  baseline deviations.
- Full CTest: 29 tests executed successfully, 0 failed, and 2 embedding-dependent tests were
  disabled without a configured model (31 tests registered in total).
- No project database, shared namespace, secret, service or external graph was mutated.

## Precedent Classification

### Precedent

- Git federated capability graph is authoritative; semantic discovery is advisory.
- CQRS allows relational authority with disposable non-relational read models.
- Qdrant and FalkorDB are accepted local-first derived projections with explicit fail-soft behavior.
- Shared infrastructure is mandatory for stateful server providers.

### Not a contradiction after clarification

- The local journal is a transactional outbox/change feed, not Event Sourcing: the event log is not
  the project write model's source of truth. Full Event Sourcing remains opt-in and is not adopted.

### Unverified / gated

- Final platform owner and contract id for the new cross-cutting capability.
- Accepted ADR status.
- Provisioned Axon PostgreSQL database/schema, Qdrant collection, FalkorDB graph and credentials.
- CI multi-OS behavior of future code.

## G0 Checkpoint

- Mini-goal: G0 — baseline and confrontation.
- Facts observed: authority boundaries, existing capabilities, missing architecture and baseline
  defects are recorded above.
- Changes performed: read-only discovery, reference-repository analysis, this evidence report and
  the consolidated study; no product code or authoritative external state changed.
- Tests executed: Release configure/build with `-j2`; CTest result is recorded above.
- Independent verifier: the first combined review and two focused G0 reviews rejected concrete
  evidence defects; after correction, the focused G0 re-verification returned `ACCEPT`.
- Risks/gaps: Axon-first lock fallback, missing pinned corpus validator, preexisting standards gaps,
  proposed cross-cutting ownership and the existing `group_impact` defect.
- Functional percentage: 0% of the new production capability; discovery/baseline only.
- Rollback: remove or revert the future isolated G0 commit; no database or service rollback needed.
- Next action: isolated G0 commit, followed by correction and verification of draft G1.
- Human authority: no authority is needed for G0; ADR and capability-intake approval are required
  before architecture implementation begins.

## Acceptance and Verification Contract

G0 is accepted only after a verifier independently checks this baseline, the study and its G0 event
evidence. The FR/NFR list and executable mini-goal contracts belong to draft G1 and are intentionally
not authoritative or part of the isolated G0 commit. Completion of the overall goal will require
requirement-by-requirement evidence, not only a green aggregate suite.
