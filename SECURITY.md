# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| latest  | ✅ |

## Scope

Axon runs entirely locally. It does not make outbound network calls, store data remotely, or transmit source code. The primary security concerns are:

- **Local file access:** axon reads all source files in the indexed directory
- **DuckDB file:** the index contains symbols, paths, and embeddings — treat it as sensitive if the codebase is sensitive
- **HTTP mode:** `axon serve --http` binds to a port; ensure it is not exposed to untrusted networks

## Reporting a Vulnerability

If you discover a security vulnerability, **do not open a public GitHub issue**.

Email `hideakiservicos@gmail.com` with:

- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (optional)

We will respond within 72 hours and keep you updated throughout the fix process. Public disclosure happens after a fix is released.
