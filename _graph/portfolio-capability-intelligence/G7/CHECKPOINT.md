# G7 Checkpoint — Notification, Remote Ingest and Reconciliation

- **Mini-goal:** started; security authority is now Keycloak Shared OIDC.
- **Facts observed:** Keycloak Shared publishes the neutral issuer under `/kc`; backend services use
  the internal cluster JWKS endpoint while tokens retain the public issuer. Each project must use a
  dedicated realm. Axon currently has no HTTP authentication middleware or remote portfolio
  endpoint, so client-provided identity cannot be trusted.
- **Changes performed:** no product code yet. Recorded the owner selection and canonical security
  sources before introducing a transport boundary.
- **Tests executed:** read-only source/configuration inspection. The second-brain semantic search
  endpoint was unavailable after its health probe; this is recorded as degraded discovery rather
  than silently treated as a successful search.
- **Independent verifier:** ACCEPT after the owner-authorized third correction. The verifier
  recompiled and confirmed rejection of future `nbf`, unsupported JOSE `crit`, malformed audience,
  incompatible JWK algorithm/operations, 512-bit RSA modulus and unsafe public exponent. It also
  confirmed private principal construction, mutex-protected bindings/grants and validation before
  store writes.
- **Risks/gaps:** realm/client creation, credentials and any live Keycloak call remain external
  mutations and separate gates. G7 will validate signed JWTs against a configured Keycloak JWKS
  document and will not accept unauthenticated headers or a static shared token.
- **Functional percentage:** 100% of bounded G7 transport/authentication criteria. Existing G5
  projector tests provide the polling/reconcile/rebuild/stale evidence; G7 adds remote push
  authentication/binding, idempotent cursor probe and guarded identity handoff.
- **Rollback:** remove this uncommitted G7 evidence only; no runtime or external state changed.
- **Next action:** commit the isolated node, then start G8 capability-signature extraction.
- **Human authority needed:** none for the G7 commit. Live Keycloak realm/client registration,
  HTTP/TLS composition, credentials and deployment remain separate gates.
