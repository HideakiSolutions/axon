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
    model = os.environ.get("AXON_EMBEDDING_MODEL", "")
    if not model or not pathlib.Path(model).is_file():
        print("SKIP: AXON_EMBEDDING_MODEL is required for hybrid memory search")
        return 77

    with tempfile.TemporaryDirectory(prefix="axon-memory-search-") as temporary:
        root = pathlib.Path(temporary)
        (root / "package.json").write_text(
            '{"name":"axon-memory-search-smoke","private":true}\n', encoding="utf-8"
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
            return json.loads(result["content"][0]["text"]), bool(result.get("isError"))

        tools = request("tools/list", {})["tools"]
        schemas = {tool["name"]: tool["inputSchema"] for tool in tools}
        authority_schema = schemas["save_observation"]["properties"]["authority"]
        require(authority_schema["minimum"] == 0.5, "authority minimum is not documented")
        require(authority_schema["maximum"] == 2.0, "authority maximum is not documented")

        observations = (
            ("Authentication middleware validates ordinary bearer tokens.", 1.0, ["eval"]),
            ("AXON_EXACT_SENTINEL retries the durable pending write queue.", 1.0, ["eval", "queue"]),
            ("Authority boost sentinel for project memory ranking.", 99.0, ["eval"]),
            ("This row must be excluded by tag filtering.", 1.0, ["other"]),
        )
        saved_ids: list[int] = []
        for content, authority, tags in observations:
            saved, failed = call(
                "save_observation", {"content": content, "authority": authority, "tags": tags}
            )
            require(not failed, f"save_observation failed: {saved}")
            require(0.5 <= saved["authority"] <= 2.0, "effective authority is not bounded")
            saved_ids.append(saved["observation_id"])

        results, failed = call(
            "search_memory", {"query": "AXON_EXACT_SENTINEL durable queue", "tags": ["eval"], "limit": 4}
        )
        require(not failed, f"search_memory failed: {results}")
        require(results, "hybrid search returned no results")
        require(all("eval" in item["tags"] for item in results), "all-tags filter regressed")

        exact = next((item for item in results if item["observation_id"] == saved_ids[1]), None)
        require(exact is not None, "exact lexical observation was not retrieved")
        require(exact["lexical_rank"] is not None, "lexical rank evidence is missing")
        for item in results:
            for field in ("semantic_rank", "lexical_rank", "rrf_score", "score", "authority"):
                require(field in item, f"ranking evidence field {field} is missing")

        boosted, failed = call(
            "search_memory", {"query": "authority boost sentinel", "tags": ["eval"], "limit": 4}
        )
        require(not failed, f"authority search failed: {boosted}")
        boosted_item = next((item for item in boosted if item["observation_id"] == saved_ids[2]), None)
        require(boosted_item is not None, "boosted observation was not retrieved")
        require(boosted_item["authority"] == 2.0, "authority clamp was not persisted")

        server.stdin.close()
        return_code = server.wait(timeout=15)
        stderr = server.stderr.read() if server.stderr is not None else ""
        require(return_code == 0, f"axon serve failed: {stderr}")

    print("mcp_memory_search_ok=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
