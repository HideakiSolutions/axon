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
    ts_csharp ts_php ts_dart ts_java ts_bash ts_cpp ts_kotlin ts_vue ts_lua
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
