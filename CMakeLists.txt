cmake_minimum_required(VERSION 3.20)
project(axon VERSION 0.5.1 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

# ── Version + git SHA ────────────────────────────────────────────────────────
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE AXON_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT AXON_GIT_SHA)
    set(AXON_GIT_SHA "unknown")
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

# ── DuckDB (pre-built shared lib from GitHub releases) ───────────────────────
# Linux ships libduckdb.so; macOS ships libduckdb.dylib. The release-tarball
# layouts (and the CI download steps in build.yml / release.yml) keep both
# under the same third_party/duckdb/lib/ path so the only thing that varies
# is the file extension.
if(APPLE)
    set(AXON_DUCKDB_LIB ${CMAKE_SOURCE_DIR}/third_party/duckdb/lib/libduckdb.dylib)
else()
    set(AXON_DUCKDB_LIB ${CMAKE_SOURCE_DIR}/third_party/duckdb/lib/libduckdb.so)
endif()
add_library(duckdb_lib SHARED IMPORTED)
set_target_properties(duckdb_lib PROPERTIES
    IMPORTED_LOCATION ${AXON_DUCKDB_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/third_party/duckdb/lib
)
# Ensure the shared lib is found at runtime (DT_RUNPATH on Linux, LC_RPATH on macOS).
set(CMAKE_BUILD_RPATH ${CMAKE_SOURCE_DIR}/third_party/duckdb/lib)

# ── tree-sitter core ─────────────────────────────────────────────────────────
add_library(tree_sitter_lib STATIC
    third_party/tree-sitter/lib/src/lib.c
)
target_include_directories(tree_sitter_lib PUBLIC
    third_party/tree-sitter/lib/include
    third_party/tree-sitter/lib/src
)

# ── tree-sitter language grammars ────────────────────────────────────────────
set(TS_INCLUDE third_party/tree-sitter/lib/include)

add_library(ts_typescript STATIC
    third_party/tree-sitter-typescript/typescript/src/parser.c
    third_party/tree-sitter-typescript/typescript/src/scanner.c
)
target_include_directories(ts_typescript PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-typescript/typescript/src)

add_library(ts_javascript STATIC
    third_party/tree-sitter-javascript/src/parser.c
    third_party/tree-sitter-javascript/src/scanner.c
)
target_include_directories(ts_javascript PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-javascript/src)

add_library(ts_python STATIC
    third_party/tree-sitter-python/src/parser.c
    third_party/tree-sitter-python/src/scanner.c
)
target_include_directories(ts_python PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-python/src)

add_library(ts_rust STATIC
    third_party/tree-sitter-rust/src/parser.c
    third_party/tree-sitter-rust/src/scanner.c
)
target_include_directories(ts_rust PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-rust/src)

add_library(ts_go STATIC
    third_party/tree-sitter-go/src/parser.c
)
target_include_directories(ts_go PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-go/src)

add_library(ts_csharp STATIC
    third_party/tree-sitter-c-sharp/src/parser.c
    third_party/tree-sitter-c-sharp/src/scanner.c
)
target_include_directories(ts_csharp PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-c-sharp/src)

add_library(ts_php STATIC
    third_party/tree-sitter-php/php/src/parser.c
    third_party/tree-sitter-php/php/src/scanner.c
)
target_include_directories(ts_php PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-php/php/src)

add_library(ts_dart STATIC
    third_party/tree-sitter-dart/src/parser.c
    third_party/tree-sitter-dart/src/scanner.c
)
target_include_directories(ts_dart PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-dart/src)

add_library(ts_java STATIC
    third_party/tree-sitter-java/src/parser.c
)
target_include_directories(ts_java PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-java/src)

add_library(ts_bash STATIC
    third_party/tree-sitter-bash/src/parser.c
    third_party/tree-sitter-bash/src/scanner.c
)
target_include_directories(ts_bash PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-bash/src)

add_library(ts_cpp STATIC
    third_party/tree-sitter-cpp/src/parser.c
    third_party/tree-sitter-cpp/src/scanner.c
)
target_include_directories(ts_cpp PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-cpp/src)

add_library(ts_kotlin STATIC
    third_party/tree-sitter-kotlin/src/parser.c
    third_party/tree-sitter-kotlin/src/scanner.c
)
target_include_directories(ts_kotlin PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-kotlin/src)

add_library(ts_vue STATIC
    third_party/tree-sitter-vue/src/parser.c
    third_party/tree-sitter-vue/src/scanner.cc
)
target_include_directories(ts_vue PUBLIC ${TS_INCLUDE}
    third_party/tree-sitter-vue/src)

# ── llama.cpp ────────────────────────────────────────────────────────────────
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp)

# ── blake3 ───────────────────────────────────────────────────────────────────
# The hand-written assembly variants are x86-64-only; on arm64 (macOS Apple
# Silicon, Linux aarch64) they fail to assemble. The portable C path and the
# dispatcher already cover those platforms — the dispatcher's runtime CPU
# detection never reaches the SIMD code path on arm64, so dropping the .S
# files there is safe and matches BLAKE3's upstream build.
set(BLAKE3_SOURCES
    third_party/blake3/c/blake3.c
    third_party/blake3/c/blake3_dispatch.c
    third_party/blake3/c/blake3_portable.c
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
    list(APPEND BLAKE3_SOURCES
        third_party/blake3/c/blake3_sse2_x86-64_unix.S
        third_party/blake3/c/blake3_sse41_x86-64_unix.S
        third_party/blake3/c/blake3_avx2_x86-64_unix.S
        third_party/blake3/c/blake3_avx512_x86-64_unix.S
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64)")
    # Optional: arm64 NEON acceleration via the portable-headers neon source.
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/blake3/c/blake3_neon.c)
        list(APPEND BLAKE3_SOURCES third_party/blake3/c/blake3_neon.c)
    endif()
endif()
add_library(blake3_lib STATIC ${BLAKE3_SOURCES})
target_include_directories(blake3_lib PUBLIC third_party/blake3/c)
# On arm64 we lose the runtime SIMD dispatch tables; tell BLAKE3 to omit
# the SSE/AVX entry points so the dispatcher links against the C fallback.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
    target_compile_definitions(blake3_lib PRIVATE
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512
    )
endif()

# ── axon executable ──────────────────────────────────────────────────────────
file(GLOB_RECURSE AXON_SOURCES src/*.cpp)

add_executable(axon ${AXON_SOURCES})

target_include_directories(axon PRIVATE
    src/
    ${CMAKE_BINARY_DIR}/generated
    third_party/tree-sitter/lib/include
    third_party/duckdb
    third_party/llama.cpp/include
    third_party/nlohmann-json/single_include
)

target_link_libraries(axon PRIVATE
    duckdb_lib
    tree_sitter_lib
    ts_typescript
    ts_javascript
    ts_python
    ts_rust
    ts_go
    ts_csharp
    ts_php
    ts_dart
    ts_java
    ts_bash
    ts_cpp
    ts_kotlin
    ts_vue
    llama
    ggml
    blake3_lib
)

# Linux DuckDB pre-built lib uses old GCC ABI (std::string without __cxx11
# inline namespace). On Apple platforms the macOS prebuilt links against
# libc++ which doesn't honor this define, and forcing it would break
# inline-namespace mangling — so gate to non-Apple.
if(NOT APPLE)
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()

target_compile_options(axon PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

# ── tests ────────────────────────────────────────────────────────────────────
# Default ON only when this is the top-level project, so downstream embedders
# that vendor axon don't accidentally pull in GoogleTest. Toggle with
# `cmake -S . -B build -DAXON_BUILD_TESTS=OFF`.
option(AXON_BUILD_TESTS "Build axon unit tests" ${PROJECT_IS_TOP_LEVEL})
if(AXON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
