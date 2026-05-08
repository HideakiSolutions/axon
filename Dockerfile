# syntax=docker/dockerfile:1.6
#
# Multi-stage build for the axon context engine.
#
# Stage 1 (builder): Ubuntu 22.04 with the build toolchain + recursive
#   submodules → produces /opt/axon/build/axon and a copy of libduckdb.
# Stage 2 (runtime): debian:12-slim with just libstdc++ and ca-certificates;
#   bundles the binary, libduckdb, and the example fixtures so a
#   `docker run` user can `axon index /work` against a mounted project.
#
# Build:    docker build -t axon:dev .
# Run:      docker run --rm -v "$PWD":/work axon:dev index /work
# Help:     docker run --rm axon:dev help

# ──────────────────────────────────────────────────────────────────────────
# Stage 1 — builder
# ──────────────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/axon
COPY . /opt/axon

# Some submodules may already be present from `git clone --recursive` on the
# host; if not, init them inside the build context. Project policy enforces
# -j2 on developer hosts because of llama.cpp + tree-sitter contention with
# concurrent MCP servers — inside Docker the host competition isn't there
# but two-way parallelism is still a sane CI cap.
RUN if [ -d .git ]; then git submodule update --init --recursive; fi
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -GNinja && \
    cmake --build build --target axon -j 2

# ──────────────────────────────────────────────────────────────────────────
# Stage 2 — runtime
# ──────────────────────────────────────────────────────────────────────────
FROM debian:12-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

# Layout matches the release tarball staged by .github/workflows/release.yml
# so users coming from the install.sh path find the same /opt/axon/{bin,lib}.
WORKDIR /opt/axon
COPY --from=builder /opt/axon/build/axon                   /opt/axon/bin/axon
COPY --from=builder /opt/axon/third_party/duckdb/lib/libduckdb.so /opt/axon/lib/
COPY --from=builder /opt/axon/examples                     /opt/axon/examples
COPY --from=builder /opt/axon/README.md                    /opt/axon/README.md
COPY --from=builder /opt/axon/LICENSE                      /opt/axon/LICENSE

ENV PATH=/opt/axon/bin:${PATH} \
    LD_LIBRARY_PATH=/opt/axon/lib:${LD_LIBRARY_PATH}

# Embedding model is intentionally NOT baked in — Q4_K_M GGUF weighs ~150 MiB
# and licensing varies. Mount or download at runtime via AXON_EMBEDDING_MODEL
# (see docs/en/getting-started.md) or run without embeddings (axon index will
# warn-and-skip).

WORKDIR /work
ENTRYPOINT ["/opt/axon/bin/axon"]
CMD ["help"]
