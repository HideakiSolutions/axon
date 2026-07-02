# Axon-Primary / RTK-Optional Guide Evidence - 2026-07-01

## Scope

- Add release/migration guidance for using Axon as the primary context and shell-output optimization layer.
- Keep RTK positioned as optional compatibility/fallback tooling.
- Document agent routing rules that prefer Axon tools before raw file reads.
- Document shell filter commands, CCR recovery, JSON metrics, aggregate benchmark runner, and release gate.

## Artifacts

- English guide: `docs/en/axon-primary-rtk-optional.md`
- Portuguese guide: `docs/pt-br/axon-primary-rtk-optional.md`
- README link: `README.md`
- Getting-started links: `docs/en/getting-started.md`, `docs/pt-br/getting-started.md`

## Fidelity Checks

- Guide points context expansion to `get_context_capsule`, `get_skeleton`, `expand_command`, and `artifact_retrieve`.
- Guide points shell-output optimization to `axon filter ... --metrics=json`.
- Guide keeps RTK as fallback only when an Axon filter family does not exist or during benchmark comparison.
- Guide uses the existing aggregate runner as the release comparison gate.
