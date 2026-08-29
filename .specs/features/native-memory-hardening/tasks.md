# Native Memory Hardening — Tasks

```yaml
tasks:
  - id: T1
    name: "Harden the pending-write spool"
    description: "Make queue claims recoverable and at-least-once without changing hook inputs."
    input_contract:
      files:
        - ".specs/features/native-memory-hardening/design.md"
        - "src/mcp/server.cpp"
        - "tests/smoke/test_queue_drain.sh"
      dependencies: []
    output_contract:
      files:
        - "src/mcp/server.cpp"
        - "src/core/pending_writes.hpp"
        - "src/core/pending_writes.cpp"
        - "tests/unit/test_pending_writes.cpp"
        - "tests/CMakeLists.txt"
      artifacts:
        - "queue failure/recovery tests passing"
    constraints:
      - "C++20; no dependency additions"
      - "at-least-once and project-local"
    acceptance_criteria:
      - "claimed batch survives indexing failure"
      - "stale processing claim is replayed"
      - "duplicate paths are indexed once per drain"
    execution_mode: isolated
    verify_step:
      command: "ctest --test-dir build --output-on-failure -R 'mcp|queue'"
      expected: "0 failed"
      fallback: "bash tests/smoke/test_queue_drain.sh"

  - id: T2
    name: "Add idempotent sessions and typed handoffs"
    description: "Extend Dialogue Layer schema, core API, MCP tools, and state-machine tests."
    input_contract:
      files:
        - ".specs/features/native-memory-hardening/design.md"
        - "src/core/db.cpp"
        - "src/core/dialogue.hpp"
        - "src/core/dialogue.cpp"
        - "src/mcp/server.cpp"
        - "tests/unit/test_dialogue.cpp"
      dependencies: [T1]
    output_contract:
      files:
        - "src/core/db.cpp"
        - "src/core/dialogue.hpp"
        - "src/core/dialogue.cpp"
        - "src/mcp/server.cpp"
        - "tests/unit/test_dialogue.cpp"
      artifacts:
        - "additive schema and MCP contracts"
    constraints:
      - "existing session_start clients remain compatible"
      - "working_directory remains under project_root"
    acceptance_criteria:
      - "same session idempotency key returns same id"
      - "valid handoff lifecycle succeeds and invalid transitions fail"
      - "MCP schemas expose only additive inputs/tools"
    execution_mode: isolated
    verify_step:
      command: "ctest --test-dir build --output-on-failure -R dialogue"
      expected: "0 failed"
      fallback: "./build/test_dialogue"

  - id: T3
    name: "Implement explainable hybrid memory retrieval"
    description: "Fuse semantic and lexical observation candidates with authority-bounded RRF."
    input_contract:
      files:
        - ".specs/features/native-memory-hardening/design.md"
        - "src/core/db.cpp"
        - "src/mcp/server.cpp"
        - "tests/unit/test_semantic.cpp"
      dependencies: [T2]
    output_contract:
      files:
        - "src/core/db.cpp"
        - "src/core/memory_search.hpp"
        - "src/core/memory_search.cpp"
        - "src/mcp/server.cpp"
        - "tests/unit/test_memory_search.cpp"
        - "CMakeLists.txt"
      artifacts:
        - "deterministic RRF and authority tests"
    constraints:
      - "candidate sets bounded to 500"
      - "authority clamped to [0.5, 2.0]"
    acceptance_criteria:
      - "exact lexical match can complement semantic rank"
      - "ranking evidence is returned"
      - "tie order is deterministic"
    execution_mode: isolated
    verify_step:
      command: "ctest --test-dir build --output-on-failure -R memory_search"
      expected: "0 failed"
      fallback: "./build/test_memory_search"

  - id: T4
    name: "Validate and document native memory hardening"
    description: "Add eval fixtures, reconcile docs, and run repository quality gates."
    input_contract:
      files:
        - ".specs/features/native-memory-hardening/spec.md"
        - ".specs/features/native-memory-hardening/design.md"
        - "README.md"
        - "docs/en/api-reference.md"
        - "docs/pt-br/getting-started.md"
      dependencies: [T1, T2, T3]
    output_contract:
      files:
        - "evals/README.md"
        - "evals/memory-retrieval.jsonl"
        - "evals/run_memory_retrieval.py"
        - "README.md"
        - "docs/en/api-reference.md"
        - "docs/en/native-memory.md"
        - "docs/pt-br/native-memory.md"
      artifacts:
        - "full test and documentation evidence"
    constraints:
      - "no release or deployment in this task"
    acceptance_criteria:
      - "eval corpus covers exact, semantic, authority, and tie cases"
      - "English and Portuguese contracts agree"
      - "full deterministic quality gates pass"
    execution_mode: isolated
    verify_step:
      command: "cmake --build build -j2 && ctest --test-dir build --output-on-failure"
      expected: "all tests pass"
      fallback: "cmake --build build -j1 && ctest --test-dir build --output-on-failure"
```
