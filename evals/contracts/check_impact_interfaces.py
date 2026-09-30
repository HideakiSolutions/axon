#!/usr/bin/env python3
"""Exercise stage 4/5 CLI and MCP through two real local indexes."""
import json
import os
from pathlib import Path
import subprocess
import tempfile


def mcp(axon, root, env, name, arguments):
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": name, "arguments": arguments}}
    payload = json.dumps(request).encode()
    wire = b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload
    output = subprocess.check_output([axon, "serve"], input=wire, cwd=root,
                                     env=env, stderr=subprocess.DEVNULL, timeout=30)
    rpc = json.loads(output.split(b"\r\n\r\n", 1)[1])
    assert not rpc.get("error") and not rpc["result"].get("isError"), rpc
    return json.loads(rpc["result"]["content"][0]["text"])


def cli(axon, root, env, *args):
    return json.loads(subprocess.check_output([axon, *args], cwd=root, env=env, timeout=30))


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--axon", required=True)
    axon = str(Path(parser.parse_args().axon).resolve())
    with tempfile.TemporaryDirectory(prefix="axon-impact-smoke-") as temp:
        base = Path(temp)
        env = dict(os.environ, AXON_REGISTRY_DIR=str(base / "registry"))
        env.pop("AXON_EMBEDDING_DEVICE", None)
        env.pop("AXON_EMBEDDING_MODEL", None)
        orders, billing = base / "orders", base / "billing"
        for root in (orders, billing):
            root.mkdir()
            (root / ".git").mkdir()
            (root / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
        (orders / "api.ts").write_text(
            "const app = express();\n"
            "function createOrder(req, res) {\n"
            "  res.json({id: req.body.id, status: 'created'});\n"
            "}\n"
            "app.post('/v1/orders', function createOrder(req, res) { createOrder(req, res); });\n")
        (billing / "client.ts").write_text(
            "function readOrder(req) {\n"
            " const response = http.post('https://orders.example.test/v1/orders', req.body);\n"
            " return response.data.amount;\n"
            "}\n")
        (billing / "trace.ts").write_text(
            "function saveName(req) {\n"
            " const name = req.body.name.trim();\n"
            " db.save(name);\n"
            "}\n")
        for root in (orders, billing):
            subprocess.run([axon, "index", str(root)], cwd=root, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)

        communities = cli(axon, orders, env, "symbol-communities")
        assert communities == mcp(axon, orders, env, "symbol_communities", {})
        assert communities["communities"], communities

        flow = cli(axon, orders, env, "execution-flow")
        assert flow == mcp(axon, orders, env, "execution_flow", {})
        assert any(p["output_kind"] == "return" for p in flow["paths"]), flow

        shape = cli(axon, orders, env, "api-shape")
        assert shape == mcp(axon, orders, env, "api_shape", {})
        assert any(f["field"] == "amount" and f["verdict"] == "proven"
                   for f in shape["findings"]), shape

        trace = cli(axon, billing, env, "data-trace", "trace.ts")
        remote_trace = mcp(axon, billing, env, "trace_data_flow", {"file": "trace.ts"})
        for transient in ("elapsed_us",):
            trace.pop(transient, None)
            remote_trace.pop(transient, None)
        assert trace == remote_trace, (trace, remote_trace)
        assert any(p["status"] == "confirmed" for p in trace["paths"]), trace
        print("PASS impact CLI/MCP: communities, execution flow, API shape, data trace")


if __name__ == "__main__":
    main()
