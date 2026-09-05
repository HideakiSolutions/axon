# Axon v1.2.16 release baseline

- Source baseline: `main` / `origin/main` at `aefa8e95aa9422468aa69bd431918d4069c5157f`.
- Version surfaces: CMake `1.2.16`, VS Code extension `1.2.16`, changelog section `1.2.16`, binary `axon 1.2.16 (build aefa8e9)`.
- Scope: Linux candidate package, VSIX, deterministic validation and publication readiness only.
- Acceptance: self-contained package, zero extension vulnerabilities, full CTest and E2E with real model, tag/release absence confirmed, hashes recorded.
- Rollback: candidate artifacts are isolated under `/opt/hideakisolutions/axon-release-candidates`; live installation is unchanged.
- Stop condition: tag/publication gate, failed required CI, or artifact validation failure.
