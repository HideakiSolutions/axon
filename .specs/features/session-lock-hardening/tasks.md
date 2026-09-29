# Session Lock Hardening — Tasks

```yaml
tasks:
  - id: T1
    name: "Implement bounded DB ownership lifecycle"
    description: "Add owner heartbeat metadata, stdio idle release, and transparent reacquisition."
    input_contract:
      files: ["src/core/registry.cpp", "src/mcp/server.cpp", "src/mcp/peer.cpp"]
      dependencies: []
    output_contract:
      files: ["src/core/registry.hpp", "src/core/registry.cpp", "src/mcp/server.hpp", "src/mcp/server.cpp"]
      verify_step:
        type: automated
        command: "ctest --test-dir build --output-on-failure -R 'test_registry|test_session_lock_lifecycle'"
        expected: "2 tests passed, 0 failed"
        fallback: "python3 tests/smoke/test_session_lock_lifecycle.py build/axon"
    acceptance_criteria:
      - "idle owner releases DB while process remains alive"
      - "next tool call reacquires ownership"
      - "active calls cannot race release"
    execution_mode: isolated

  - id: T2
    name: "Bound frozen-owner and hook waits"
    description: "Replace five-minute peer waits and unbounded hook subprocesses with deadlines."
    input_contract:
      files: ["src/mcp/peer.cpp", "scripts/hooks/axon-queue-drain.sh", "scripts/hooks/axon-queue-drain.ps1"]
      dependencies: [T1]
    output_contract:
      files: ["src/mcp/peer.cpp", "scripts/hooks/axon-queue-drain.sh", "scripts/hooks/axon-queue-drain.ps1"]
      verify_step:
        type: automated
        command: "ctest --test-dir build --output-on-failure -R 'test_queue_drain|test_session_lock_lifecycle'"
        expected: "all selected tests pass within bounded time"
        fallback: "bash tests/smoke/test_queue_drain.sh"
    acceptance_criteria:
      - "frozen owner returns an error in under three seconds with test configuration"
      - "hook kills only its own timed-out child"
    execution_mode: isolated

  - id: T3
    name: "Add lock diagnostics and delivery evidence"
    description: "Expose read-only diagnostics, reconcile docs, and run repository quality gates."
    input_contract:
      files: ["src/main.cpp", "README.md", "docs/en/troubleshooting.md", "CHANGELOG.md"]
      dependencies: [T1, T2]
    output_contract:
      files: ["src/main.cpp", "README.md", "docs/en/troubleshooting.md", "CHANGELOG.md"]
      verify_step:
        type: automated
        command: "cmake --build build -j2 && ctest --test-dir build --output-on-failure"
        expected: "build succeeds and 0 tests fail"
        fallback: "cmake --build build -j1 && ctest --test-dir build --output-on-failure -R 'registry|queue|session_lock'"
    acceptance_criteria:
      - "doctor output classifies healthy and frozen owners without tokens"
      - "public docs state lifecycle defaults and recovery procedure"
      - "full deterministic gates pass"
    execution_mode: isolated
```
