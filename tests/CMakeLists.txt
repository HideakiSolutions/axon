# GoogleTest via FetchContent — pinned to the latest stable at the time of
# bootstrap. Bumping is a Dependabot concern (gitsubmodule isn't applicable
# to FetchContent, so we drive this manually as part of release prep).
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

# Watcher tests — filesystem-only (no DuckDB/llama); language_from_extension
# comes from the shared parser objects, the skip-list from skip_dirs.cpp.
add_axon_test(test_watcher
    unit/test_watcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watcher_poll.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watcher_inotify.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watcher_fsevents.cpp
    ${CMAKE_SOURCE_DIR}/src/core/watcher_win32.cpp
    ${CMAKE_SOURCE_DIR}/src/core/skip_dirs.cpp
)
if(APPLE)
    target_link_libraries(test_watcher PRIVATE "-framework CoreServices")
endif()

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

add_axon_test(test_pending_writes
    unit/test_pending_writes.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pending_writes.cpp
)

add_axon_test(test_memory_search
    unit/test_memory_search.cpp
    ${CMAKE_SOURCE_DIR}/src/core/memory_search.cpp
)

add_test(
    NAME test_memory_retrieval_eval
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/evals/run_memory_retrieval.py
            --axon $<TARGET_FILE:axon>
            --dataset ${CMAKE_SOURCE_DIR}/evals/memory-retrieval.jsonl
)
set_tests_properties(test_memory_retrieval_eval PROPERTIES SKIP_RETURN_CODE 77)

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

add_executable(test_call_resolution unit/test_call_resolution.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/call_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
)
target_include_directories(test_call_resolution PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/blake3/c
)
target_link_libraries(test_call_resolution PRIVATE
    blake3_lib
    duckdb_lib
    GTest::gtest_main
)
add_test(NAME test_call_resolution COMMAND test_call_resolution)

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
if(WIN32)
    # telemetry.cpp's try_post_http speaks winsock; the axon target links
    # ws2_32 for the same reason (root CMakeLists WIN32 branch).
    target_link_libraries(test_telemetry PRIVATE ws2_32)
endif()
add_test(NAME test_telemetry COMMAND test_telemetry)

# Dialogue Layer tests require DuckDB (cannot use the parser-only axon_parser_objs target).
add_executable(test_dialogue unit/test_dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/embeddings.cpp
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
)
target_include_directories(test_dialogue PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_dialogue PRIVATE
    blake3_lib
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
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
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

# Registry tests — isolated registry plus real DuckDB read-only secondary checks.
add_executable(test_registry unit/test_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/core/registry.cpp
)
target_include_directories(test_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
)
target_link_libraries(test_registry PRIVATE
    duckdb_lib
    GTest::gtest_main
)
add_test(NAME test_registry COMMAND test_registry)

# Portfolio journal: real DuckDB, real parser, transactional fault injection.
add_executable(test_portfolio_journal unit/test_portfolio_journal.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/indexer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/routes.cpp
    ${CMAKE_SOURCE_DIR}/src/core/call_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/core/skeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/core/skip_dirs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/config.cpp
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
)
target_include_directories(test_portfolio_journal PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/blake3/c
)
target_link_libraries(test_portfolio_journal PRIVATE
    axon_parser_objs
    duckdb_lib
    blake3_lib
    GTest::gtest_main
)
add_test(NAME test_portfolio_journal COMMAND test_portfolio_journal)

# Capsule budget tests — drive assemble_capsule end-to-end; require the real
# GGUF model (GTEST_SKIP without AXON_EMBEDDING_MODEL, same as test_semantic).
add_executable(test_capsule_budget unit/test_capsule_budget.cpp
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
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
)
target_include_directories(test_capsule_budget PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_capsule_budget PRIVATE
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
add_test(NAME test_capsule_budget COMMAND test_capsule_budget)

# Semantic tests — require a real GGUF model; auto-skip via GTEST_SKIP unless
# AXON_EMBEDDING_MODEL points at the model explicitly.
add_executable(test_semantic unit/test_semantic.cpp
    ${CMAKE_SOURCE_DIR}/src/core/db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/dialogue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/embeddings.cpp
    ${CMAKE_SOURCE_DIR}/src/portfolio/domain/index_journal.cpp
)
target_include_directories(test_semantic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/single_include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
)
target_link_libraries(test_semantic PRIVATE
    blake3_lib
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
    NAME test_mcp_handoff
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_mcp_handoff.py
            $<TARGET_FILE:axon>
)

add_test(
    NAME test_mcp_pending_writes
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_mcp_pending_writes.py
            $<TARGET_FILE:axon>
)

add_test(
    NAME test_mcp_memory_search
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_mcp_memory_search.py
            $<TARGET_FILE:axon>
)
set_tests_properties(test_mcp_memory_search PROPERTIES SKIP_RETURN_CODE 77)

add_test(
    NAME test_cpp_header_impl
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_cpp_header_impl.sh $<TARGET_FILE:axon>
)

add_test(
    NAME test_http_status_codes
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_http_status_codes.sh $<TARGET_FILE:axon>
)

add_test(
    NAME test_docs_freshness
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_docs_freshness.sh
)
set_tests_properties(test_docs_freshness PROPERTIES SKIP_RETURN_CODE 77)

add_test(
    NAME test_shell_guard_hook
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_shell_guard_hook.sh
)

add_test(
    NAME test_auto_index_project_key
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_auto_index_project_key.sh
)

add_test(
    NAME test_post_edit_project_boundary
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_post_edit_project_boundary.sh
)

add_test(
    NAME test_route_pending_writes
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_route_pending_writes.sh
)

add_test(
    NAME test_queue_drain
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_queue_drain.sh
)

add_test(
    NAME test_queue_drain_registry_scope
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_queue_drain_registry_scope.sh
)

if(NOT WIN32)
    add_test(
        NAME test_session_lock_lifecycle
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_session_lock_lifecycle.py
                $<TARGET_FILE:axon>
    )
endif()

if(WIN32)
    add_test(
        NAME test_windows_queue_hooks
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
                ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_windows_queue_hooks.ps1
    )
    set_tests_properties(test_windows_queue_hooks PROPERTIES LABELS smoke)
endif()

add_test(
    NAME test_shell_filter_aggregate_benchmark
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke/test_shell_filter_aggregate_benchmark.sh $<TARGET_FILE:axon>
)
set_tests_properties(test_shell_filter_aggregate_benchmark PROPERTIES SKIP_RETURN_CODE 77)

# The bash smokes drive the built `axon` binary through subprocesses (serve,
# FIFOs, HTTP, kill timing) — behavior tests, not memory tests. They are
# covered on three platforms by build.yml; the `smoke` label lets the ASAN/
# UBSAN nightly exclude them (`ctest -LE smoke`) so it sanitizes the
# deterministic gtest units without the fragility of a whole serve under ASAN.
set_tests_properties(
    test_cli_filter_json_metrics
    test_mcp_capsule_schema
    test_mcp_handoff
    test_mcp_pending_writes
    test_mcp_memory_search
    test_cpp_header_impl
    test_http_status_codes
    test_docs_freshness
    test_shell_guard_hook
    test_auto_index_project_key
    test_post_edit_project_boundary
    test_route_pending_writes
    test_queue_drain
    test_queue_drain_registry_scope
    test_shell_filter_aggregate_benchmark
    PROPERTIES LABELS smoke)

if(NOT WIN32)
    set_tests_properties(test_session_lock_lifecycle PROPERTIES LABELS smoke)
endif()
