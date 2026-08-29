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
    with tempfile.TemporaryDirectory(prefix="axon-handoff-") as temporary:
        root = pathlib.Path(temporary)
        (root / "src").mkdir()
        (root / "package.json").write_text(
            '{"name":"axon-handoff-smoke","private":true}\n', encoding="utf-8"
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

        request_id = 0

        def request(method: str, params: dict) -> dict:
            nonlocal request_id
            request_id += 1
            payload = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
            server.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
            server.stdin.flush()
            response = json.loads(server.stdout.readline())
            require(response.get("id") == request_id, "response id mismatch")
            require("error" not in response, f"JSON-RPC error: {response}")
            return response["result"]

        def call(name: str, arguments: dict) -> tuple[dict | list, bool]:
            result = request("tools/call", {"name": name, "arguments": arguments})
            content = json.loads(result["content"][0]["text"])
            return content, bool(result.get("isError"))

        tools = request("tools/list", {})["tools"]
        schemas = {tool["name"]: tool["inputSchema"] for tool in tools}
        for name in (
            "handoff_create",
            "handoff_get",
            "handoff_list",
            "handoff_claim",
            "handoff_complete",
            "handoff_cancel",
        ):
            require(name in schemas, f"missing MCP tool {name}")
        require(
            "idempotency_key" in schemas["session_start"]["properties"],
            "session_start idempotency_key is missing",
        )

        thread, failed = call("thread_create", {"name": "handoff-smoke"})
        require(not failed, f"thread_create failed: {thread}")
        first_session, failed = call(
            "session_start",
            {"thread_id": thread["id"], "label": "first", "idempotency_key": "session-1"},
        )
        require(not failed, "session_start failed")
        replay_session, failed = call(
            "session_start",
            {"thread_id": thread["id"], "label": "replay", "idempotency_key": "session-1"},
        )
        require(not failed, "session_start replay failed")
        require(
            first_session["session_id"] == replay_session["session_id"],
            "session replay created a duplicate",
        )

        created, failed = call(
            "handoff_create",
            {
                "source_session_id": first_session["session_id"],
                "target_agent": "reviewer",
                "working_directory": "src",
                "objective": "Review native queue recovery",
                "context": "Use deterministic evidence",
                "idempotency_key": "handoff-1",
            },
        )
        require(not failed, "handoff_create failed")
        replay, failed = call(
            "handoff_create",
            {
                "source_session_id": first_session["session_id"],
                "target_agent": "reviewer",
                "objective": "Ignored replay",
                "idempotency_key": "handoff-1",
            },
        )
        require(not failed, "handoff replay failed")
        require(created["handoff_id"] == replay["handoff_id"], "handoff replay duplicated state")

        handoff_id = created["handoff_id"]
        claimed, failed = call("handoff_claim", {"handoff_id": handoff_id, "claimed_by": "agent-a"})
        require(not failed and claimed["status"] == "claimed", "handoff claim failed")
        completed, failed = call(
            "handoff_complete",
            {"handoff_id": handoff_id, "claimed_by": "agent-a", "result": "verified"},
        )
        require(not failed and completed["status"] == "completed", "handoff complete failed")

        listed, failed = call("handoff_list", {"status": "completed", "target_agent": "reviewer"})
        require(not failed and len(listed) == 1, "completed handoff filter failed")

        _, escaped = call(
            "handoff_create",
            {"target_agent": "reviewer", "working_directory": "..", "objective": "escape"},
        )
        require(escaped, "cross-project working_directory was accepted")

        server.stdin.close()
        return_code = server.wait(timeout=10)
        stderr = server.stderr.read() if server.stderr is not None else ""
        require(return_code == 0, f"axon serve failed: {stderr}")

    print("mcp_handoff_ok=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
