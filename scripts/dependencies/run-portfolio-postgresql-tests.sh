#!/usr/bin/env bash
set -euo pipefail

# Documented temporary exception: this disposable PostgreSQL exists only for hermetic provider
# verification. It has no persistent volume, never joins shared-infra and is removed by the trap.
container_name="axon-portfolio-postgresql-test-$$"
image="${AXON_POSTGRES_TEST_IMAGE:-pgvector/pgvector:pg16}"
build_dir="${AXON_POSTGRES_BUILD_DIR:-build-postgresql-test}"
test_password="axon_test_only"

cleanup() {
    docker stop "${container_name}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

docker run -d --rm --name "${container_name}" --cpus 1 --memory 512m \
    -e POSTGRES_PASSWORD="${test_password}" -e POSTGRES_USER=axon_test \
    -e POSTGRES_DB=axon_portfolio_test -p 127.0.0.1::5432 "${image}" >/dev/null

for _ in $(seq 1 60); do
    if docker exec "${container_name}" pg_isready -U axon_test -d axon_portfolio_test \
        >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
docker exec "${container_name}" pg_isready -U axon_test -d axon_portfolio_test >/dev/null

host_port="$(docker port "${container_name}" 5432/tcp | sed -E 's/.*:([0-9]+)$/\1/')"
test_dsn="host=127.0.0.1 port=${host_port} dbname=axon_portfolio_test user=axon_test password=${test_password} connect_timeout=3"

AXON_POSTGRES_TEST_DSN="${test_dsn}" cmake -S . -B "${build_dir}" \
    -DAXON_BUILD_TESTS=ON -DAXON_ENABLE_POSTGRES_TESTS=ON
cmake --build "${build_dir}" --target test_portfolio_postgresql -j "${AXON_BUILD_JOBS:-2}"
AXON_POSTGRES_TEST_DSN="${test_dsn}" ctest --test-dir "${build_dir}" \
    --output-on-failure -j1 -R '^test_portfolio_postgresql$'
