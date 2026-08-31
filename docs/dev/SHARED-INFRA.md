# Portfolio shared infrastructure

Axon keeps each project `.axon/index.duckdb` independent and authoritative. The local portfolio
profile uses a derived DuckDB store. The server profile uses the governed shared PostgreSQL 16
service as its durable central projection; Qdrant and FalkorDB remain rebuildable read models.

## PostgreSQL provider development

The PostgreSQL adapter uses `libpq` and is compiled when CMake finds PostgreSQL development files.
On Ubuntu, install `libpq-dev`. Production credentials must be supplied by the governed runtime and
must never be committed, logged or embedded in public configuration.

G6 integration tests use
[`run-portfolio-postgresql-tests.sh`](../../scripts/dependencies/run-portfolio-postgresql-tests.sh).
This is a narrow exception to the shared-infra rule: the script starts a resource-bounded PostgreSQL
16/pgvector container only for a hermetic test, creates no persistent volume, does not join the
shared network and always stops/removes the container through an exit trap. It uses disposable test
credentials. This exception does not authorize an application-local runtime database.

The shared-service compatibility smoke is separate. It may create only a uniquely named temporary
Axon namespace, must use governed credential delivery and deterministic cleanup, and requires
explicit human authorization for each environment. Normal unit/integration tests must not access
`shared-postgres`.
