#!/usr/bin/env python3
"""Smoke the public MCP group_impact shape against three real local indexes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def call(axon: str, root: Path, env: dict, file: str) -> dict:
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
        "name": "group_impact", "arguments": {"file": file}}}
    payload = json.dumps(request).encode()
    wire = b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload
    response = subprocess.check_output([axon, "serve"], input=wire, cwd=root,
                                       env=env, stderr=subprocess.DEVNULL, timeout=30)
    rpc = json.loads(response.split(b"\r\n\r\n", 1)[1])
    if rpc.get("error") or rpc["result"].get("isError"):
        raise AssertionError(rpc)
    return json.loads(rpc["result"]["content"][0]["text"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--axon", required=True)
    args = parser.parse_args()
    axon = str(Path(args.axon).resolve())
    corpus = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="axon-contract-smoke-") as temp:
        root = Path(temp)
        env = os.environ.copy()
        env["AXON_REGISTRY_DIR"] = str(root / "registry")
        env.pop("AXON_EMBEDDING_DEVICE", None)
        env.pop("AXON_EMBEDDING_MODEL", None)
        for name in ("orders", "billing", "notifications"):
            checkout = root / name
            shutil.copytree(corpus / name, checkout)
            (checkout / ".git").mkdir()
            (checkout / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
            subprocess.run([axon, "index", str(checkout)], cwd=checkout, env=env,
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)

        checks = [
            ("orders", "api.ts", "http", "billing"),
            ("orders", "openapi.yaml", "openapi", "billing"),
            ("orders", "events.js", "topic", "billing"),
            ("orders", "grpc_client.cs", "grpc", "billing"),
        ]
        for repo, file, surface, peer in checks:
            result = call(axon, root / repo, env, file)
            assert result["typed_evidence_state"] == "indexed", result
            assert result["contract_result_state"] == "confirmed_links", result
            assert any(link["surface"] == surface and
                       (link["consumer"]["repo"] == peer or
                        link["provider"]["repo"] == peer)
                       for link in result["typed_contracts"]), (repo, file, result)
            if surface == "topic":
                assert all(link["consumer"]["file"] != "events.go"
                           for link in result["typed_contracts"]), result
            assert "heuristic_candidates" in result and "cross_repo_impact" in result
            print(f"PASS {surface}: {repo}/{file}")

        dynamic = call(axon, root / "notifications", env, "dynamic.cs")
        assert dynamic["contract_result_state"] == "no_confirmed_link", dynamic
        assert {e["surface"] for e in dynamic["unknown_contracts"]} >= {"http", "topic"}
        print("PASS dynamic: unknown evidence and no_confirmed_link")


if __name__ == "__main__":
    main()
