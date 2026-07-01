# GoogleTest via FetchContent — pinned to the latest stable at the time of
# bootstrap. Bumping is a Dependabot concern (gitsubmodule isn't applicable
# to FetchContent, so we drive this manually as part of release prep).
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Use the library's own ABI flags rather than fighting our project-wide
# _GLIBCXX_USE_CXX11_ABI=0 — gtest's own headers are simple enough to be
# ABI-agnostic in practice, and pinning matters more than minor flag drift.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# axon_parser_objs — re-compile only the parser + blake3 wrappers so unit
# tests don't have to bring in DuckDB/llama.cpp. We link against the same
# tree-sitter static libs as the main binary.
add_library(axon_parser_objs OBJECT
    ${CMAKE_SOURCE_DIR}/src/parser/parser.cpp
)
target_include_directories(axon_parser_objs PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/tree-sitter/lib/include
)
target_link_libraries(axon_parser_objs PUBLIC
    tree_sitter_lib
    ts_typescript ts_javascript ts_python ts_rust ts_go
    ts_csharp ts_php ts_dart ts_java ts_bash ts_cpp ts_kotlin ts_vue ts_lua ts_nix
    ts_ruby ts_swift ts_scala
    blake3_lib
)

# Helper: register a gtest binary with shared parser objs and CTest entry.
function(add_axon_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        axon_parser_objs
        GTest::gtest_main
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_axon_test(test_parser_smoke unit/test_parser_smoke.cpp)

# Compress tests — pure function, no DuckDB or llama.cpp required.
add_axon_test(test_compress
    unit/test_compress.cpp
    ${CMAKE_SOURCE_DIR}/src/core/compress.cpp
)

add_axon_test(test_shell_filter
    unit/test_shell_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/core/shell_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/core/compress.cpp
)
target_include_directories(test_shell_filter PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
)

# CCR artifact tests require DuckDB and BLAKE3.
add_executable(test_ccr unit/test_ccr.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ccr.cpp
)
target_include_directories(test_ccr PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/blake3/c
)
target_link_libraries(test_ccr PRIVATE
    duckdb_lib
    blake3_lib
    GTest::gtest_main
)
add_test(NAME test_ccr COMMAND test_ccr)

# Telemetry aggregation tests require DuckDB, but not llama.cpp.
add_executable(test_telemetry unit/test_telemetry.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/telemetry.cpp
)
target_include_directories(test_telemetry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
)
target_link_libraries(test_telemetry PRIVATE
    duckdb_lib
    GTest::gtest_main
)
add_test(NAME test_telemetry COMMAND test_telemetry)

# Dialogue Layer tests require DuckDB (cannot use the parser-only axon_parser_objs target).
add_executable(test_dialogue unit/test_dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/embeddings.cpp
)
target_include_directories(test_dialogue PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_dialogue PRIVATE
    duckdb_lib
    llama
    ggml
    GTest::gtest_main
)
add_test(NAME test_dialogue COMMAND test_dialogue)

# Objectives test — token reduction + memory correctness (no model required)
add_executable(test_objectives unit/test_objectives.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/embeddings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/capsule.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ccr.cpp
    ${CMAKE_SOURCE_DIR}/src/core/compress.cpp
    ${CMAKE_SOURCE_DIR}/src/core/shell_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/core/graph.cpp
    ${CMAKE_SOURCE_DIR}/src/core/skeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/core/git.cpp
)
target_include_directories(test_objectives PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_objectives PRIVATE
    tree_sitter_lib
    ts_typescript ts_javascript ts_python ts_rust ts_go
    ts_csharp ts_php ts_dart ts_java ts_bash ts_cpp ts_kotlin ts_vue
    ts_lua ts_nix ts_ruby ts_swift ts_scala
    blake3_lib
    duckdb_lib
    llama
    ggml
    GTest::gtest_main
)
add_test(NAME test_objectives COMMAND test_objectives)

# Semantic tests — require a real GGUF model; auto-skip via GTEST_SKIP unless
# AXON_EMBEDDING_MODEL points at the model explicitly.
add_executable(test_semantic unit/test_semantic.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/embeddings.cpp
)
target_include_directories(test_semantic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_semantic PRIVATE
    duckdb_lib
    llama
    ggml
    GTest::gtest_main
)
add_test(NAME test_semantic COMMAND test_semantic)

add_test(
    NAME test_cli_filter_json_metrics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_cli_filter_json_metrics.sh $<TARGET_FILE:axon>
)

add_test(
    NAME test_mcp_capsule_schema
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_mcp_capsule_schema.sh $<TARGET_FILE:axon>
)

add_test(
    NAME test_docs_freshness
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_docs_freshness.sh
)

add_test(
    NAME test_shell_guard_hook
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_shell_guard_hook.sh
)

add_test(
    NAME test_shell_filter_aggregate_benchmark
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_shell_filter_aggregate_benchmark.sh $<TARGET_FILE:axon>
)
