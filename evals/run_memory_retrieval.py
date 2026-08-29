#!/usr/bin/env python3
"""Compare semantic-only observation ranking with Axon's hybrid RRF ranking."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import statistics
import subprocess
import tempfile
import time


OBSERVATIONS = (
    ("queue", "AXON_QUEUE_SENTINEL is the durable pending-writes spool for captured file changes.", 1.0),
    ("handoff", "A typed handoff transfers implementation context and ownership to another coding agent.", 1.0),
    ("authority", "Project decisions may carry bounded authority for ranking, never automatic authorization.", 1.1),
    ("capture", "Interrupted indexing keeps captured paths pending so a later serve can retry them.", 1.0),
    ("noise-auth", "Bearer token middleware authenticates HTTP requests and rotates credentials.", 1.0),
    ("noise-queue", "A user interface queue renders transient notification cards in the browser.", 1.0),
    ("tie-a", "Deterministic duplicate ranking fixture.", 1.0),
    ("tie-b", "Deterministic duplicate ranking fixture.", 1.0),
)


def reciprocal_rank(ranking: list[str], relevant: str) -> float:
    try:
        return 1.0 / (ranking.index(relevant) + 1)
    except ValueError:
        return 0.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--axon", required=True, type=pathlib.Path)
    parser.add_argument("--dataset", type=pathlib.Path, default=pathlib.Path(__file__).with_name("memory-retrieval.jsonl"))
    args = parser.parse_args()

    model = os.environ.get("AXON_EMBEDDING_MODEL", "")
    if not model or not pathlib.Path(model).is_file():
        print("SKIP: AXON_EMBEDDING_MODEL is required for the retrieval evaluation")
        return 77

    cases = [json.loads(line) for line in args.dataset.read_text(encoding="utf-8").splitlines() if line.strip()]
    with tempfile.TemporaryDirectory(prefix="axon-memory-eval-") as temporary:
        root = pathlib.Path(temporary)
        (root / "package.json").write_text('{"name":"axon-memory-eval","private":true}\n', encoding="utf-8")
        environment = os.environ.copy()
        environment["AXON_REGISTRY_DIR"] = str(root / "registry")
        for command in ((str(args.axon.resolve()), "init", str(root)), (str(args.axon.resolve()), "index", str(root), "--force")):
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=environment, text=True)

        server = subprocess.Popen(
            [str(args.axon.resolve()), "serve"], cwd=root, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        assert server.stdin is not None and server.stdout is not None
        request_id = 0

        def call(name: str, arguments: dict) -> object:
            nonlocal request_id
            request_id += 1
            payload = {"jsonrpc":"2.0", "id":request_id, "method":"tools/call", "params":{"name":name, "arguments":arguments}}
            server.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
            server.stdin.flush()
            response = json.loads(server.stdout.readline())
            if "error" in response:
                raise RuntimeError(response["error"])
            result = response["result"]
            body = json.loads(result["content"][0]["text"])
            if result.get("isError"):
                raise RuntimeError(body)
            return body

        id_to_key: dict[int, str] = {}
        for key, content, authority in OBSERVATIONS:
            saved = call("save_observation", {"content":content, "authority":authority, "tags":["retrieval-eval"]})
            assert isinstance(saved, dict)
            id_to_key[int(saved["observation_id"])] = key

        semantic_recall: list[float] = []
        hybrid_recall: list[float] = []
        semantic_rr: list[float] = []
        hybrid_rr: list[float] = []
        latencies_ms: list[float] = []
        details: list[dict] = []
        for case in cases:
            started = time.perf_counter()
            result = call("search_memory", {"query":case["query"], "tags":["retrieval-eval"], "limit":len(OBSERVATIONS)})
            latencies_ms.append((time.perf_counter() - started) * 1000.0)
            assert isinstance(result, list)
            hybrid = [id_to_key[int(item["observation_id"])] for item in result]
            semantic_items = [item for item in result if item.get("semantic_rank") is not None]
            semantic_items.sort(key=lambda item: (int(item["semantic_rank"]), int(item["observation_id"])))
            semantic = [id_to_key[int(item["observation_id"])] for item in semantic_items]
            k = int(case["k"])
            relevant = str(case["relevant"])
            semantic_recall.append(float(relevant in semantic[:k]))
            hybrid_recall.append(float(relevant in hybrid[:k]))
            semantic_rr.append(reciprocal_rank(semantic, relevant))
            hybrid_rr.append(reciprocal_rank(hybrid, relevant))
            details.append({"case":case["case"], "query":case["query"], "relevant":relevant, "semantic":semantic[:k], "hybrid":hybrid[:k]})

        server.stdin.close()
        return_code = server.wait(timeout=15)
        stderr = server.stderr.read() if server.stderr is not None else ""
        if return_code != 0:
            raise RuntimeError(f"axon serve failed: {stderr}")

    report = {
        "cases":len(cases),
        "semantic":{"recall_at_k":statistics.mean(semantic_recall), "mrr":statistics.mean(semantic_rr)},
        "hybrid":{"recall_at_k":statistics.mean(hybrid_recall), "mrr":statistics.mean(hybrid_rr)},
        "latency_ms":{"median":statistics.median(latencies_ms), "max":max(latencies_ms)},
        "details":details,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if report["hybrid"]["recall_at_k"] < report["semantic"]["recall_at_k"]:
        return 1
    if report["hybrid"]["mrr"] < report["semantic"]["mrr"]:
        return 1
    if report["hybrid"]["recall_at_k"] < 1.0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
