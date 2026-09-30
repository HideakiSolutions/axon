#!/usr/bin/env python3
"""Compare one uncached capsule through CLI, MCP stdio and HTTP."""
import argparse
import json
import os
import socket
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path


def comparable(capsule):
    return {
        "query": capsule["query"],
        "retrieval_mode": capsule["retrieval_mode"],
        "selection": capsule["selection"],
        "token_estimate": capsule["token_estimate"],
        "pivot_files": [(item["path"], item["content"]) for item in capsule["pivot_files"]],
        "support_files": [(item["path"], item["content"]) for item in capsule["support_files"]],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--axon", type=Path, required=True)
    parser.add_argument("--query", default="Como a câmera segue o jogador e ajusta o zoom?")
    parser.add_argument("--mode", choices=("semantic", "hybrid"), default="hybrid")
    args = parser.parse_args()
    axon = str(args.axon.resolve())
    project = str(args.project.resolve())
    env = dict(os.environ, AXON_EMBEDDING_DEVICE="cpu")

    cli = json.loads(subprocess.check_output(
        [axon, "capsule", args.query, "--no-cache", f"--retrieval-mode={args.mode}"],
        cwd=project, env=env, text=True, stderr=subprocess.DEVNULL))

    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
        "name": "get_context_capsule", "arguments": {"query": args.query,
            "retrieval_mode": args.mode, "no_cache": True}}}
    payload = json.dumps(request).encode()
    wire = b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload
    response = subprocess.check_output([axon, "serve"], input=wire, cwd=project,
                                       env=env, stderr=subprocess.DEVNULL)
    rpc = json.loads(response.split(b"\r\n\r\n", 1)[1])
    if rpc.get("error") or rpc["result"].get("isError"):
        raise RuntimeError(rpc)
    mcp = json.loads(rpc["result"]["content"][0]["text"])

    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    server = subprocess.Popen([axon, "serve", "--http", f"--port={port}",
                               "--host=127.0.0.1"], cwd=project, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        url = f"http://127.0.0.1:{port}/api/capsule?" + urllib.parse.urlencode({
            "q": args.query, "retrieval_mode": args.mode, "no_cache": "true"})
        for _ in range(50):
            try:
                with urllib.request.urlopen(url, timeout=2) as response:
                    http = json.load(response)["capsule"]
                break
            except (OSError, ValueError):
                if server.poll() is not None:
                    raise RuntimeError("HTTP server exited before capsule request")
                time.sleep(0.1)
        else:
            raise TimeoutError("HTTP capsule server did not become ready")
    finally:
        server.terminate()
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()

    expected = comparable(cli)
    assert comparable(mcp) == expected, "MCP capsule differs from CLI"
    assert comparable(http) == expected, "HTTP capsule differs from CLI"
    print(f"interfaces_equal=true mode={args.mode} tokens={cli['token_estimate']}")


if __name__ == "__main__":
    main()
