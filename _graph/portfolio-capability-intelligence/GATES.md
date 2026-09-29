# Architecture and Capability-Intake Gates

- Recorded at: 2026-08-30T16:31:34.830981010Z.
- Authority: explicit statement from the responsible owner in the active governed goal.
- ADR disposition: ADR-0003 `Accepted`.
- Capability-intake disposition: `promote` to `platform-core` approved.
- Authorized next scope: G2 additive local implementation within its closed `allowed_paths`.
- Still prohibited: external capability-graph write, shared schema/data migration, infrastructure
  mutation, secret access, security-policy change, cross-repository refactor, package publication,
  merge, deploy, tag and release.
- Semantic discovery status: unavailable/degraded; approval does not turn an absent graph match into
  an existing capability.
- Rollback: revert this local gate-record commit and stop before G2; no external state was changed.
