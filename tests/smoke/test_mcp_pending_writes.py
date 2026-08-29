#!/usr/bin/env python3
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    axon_bin = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="axon-pending-writes-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "src" / "recovered.cpp"
        source.parent.mkdir()
        source.write_text("int recovered() { return 1; }\n", encoding="utf-8")
        (root / "package.json").write_text(
            '{"name":"axon-pending-writes-smoke","private":true}\n', encoding="utf-8"
        )
        environment = os.environ.copy()
        environment["AXON_REGISTRY_DIR"] = str(root / "registry")
        subprocess.run(
            [str(axon_bin), "init", str(root)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=environment,
            text=True,
        )
        subprocess.run(
            [str(axon_bin), "index", str(root), "--force"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=environment,
            text=True,
        )

        axon_dir = root / ".axon"
        (axon_dir / "pending-writes.processing").write_text(
            f"{source}\n", encoding="utf-8"
        )
        (axon_dir / "pending-writes.processing.attempts").write_text("1\n", encoding="utf-8")

        server = subprocess.Popen(
            [str(axon_bin), "serve"],
            cwd=root,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert server.stdin is not None
        assert server.stdout is not None
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": "thread_create", "arguments": {"name": "recovery-smoke"}},
        }
        server.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        server.stdin.flush()
        response = json.loads(server.stdout.readline())
        require("error" not in response, f"JSON-RPC error: {response}")

        server.stdin.close()
        return_code = server.wait(timeout=15)
        stderr = server.stderr.read() if server.stderr is not None else ""
        require(return_code == 0, f"axon serve failed: {stderr}")
        events = [json.loads(line) for line in stderr.splitlines() if line.startswith("{")]
        recovered = [event for event in events if event.get("event") == "queue_drain_recovered"]
        require(len(recovered) == 1, f"recovery event missing: {stderr}")
        require(recovered[0].get("attempt") == 2, "recovery attempt is not observable")
        require(recovered[0].get("path_count") == 1, "recovery path count is not observable")
        require(
            not (axon_dir / "pending-writes.processing").exists(),
            "recovered claim was not acknowledged",
        )

    print("mcp_pending_writes_ok=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
