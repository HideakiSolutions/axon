# G8 Checkpoint — Capability Signature Extraction

- **Mini-goal:** accepted; ready for isolated commit.
- **Facts observed:** the v1 schema requires versioned provenance and evidence, while explicitly
  forbidding central source copies. No prior extractor existed.
- **Changes performed:** added a pure metadata input/signature model, canonical BLAKE3 fingerprints,
  deterministic normalizers and an affected-entity extractor. Added golden C++, TypeScript and
  Python fixtures; fields carry local references/digests only.
- **Tests executed:** `test_capability_signature` — 9/9 passed; complete `axon` build with `-j1`,
  `axon --version` and `git diff --check` passed.
- **Independent verifier:** initially rejected three major counterexamples: incomplete evidence
  canonicalization, validation outside an incremental impact partition, and schema-invalid metadata.
  Correction 1 now canonicalizes every evidence field, validates fail-closed and represents an empty
  incremental impact explicitly. Further owner-authorized corrections closed RFC3339, canonical
  framing, optional-presence, full schema-channel and call-neighborhood order counterexamples.
  The final independent verifier accepted the result.
- **Risks/gaps:** extractor consumes existing indexed metadata only; wiring it to the journal/projector
  belongs to later projection/query goals. Embeddings remain absent by design.
- **Functional percentage:** 100% of bounded G8 criteria.
- **Rollback:** revert the isolated G8 commit; signatures are derived and no central/provider state is written.
- **Next action:** commit G8, then begin G9 semantic read-model discovery.
- **Human authority needed:** none.
