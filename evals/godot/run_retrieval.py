#!/usr/bin/env python3
"""Run the frozen Godot retrieval set against an already indexed copy.

The marker check is a reproducible evidence proxy, not a human answer review.
Run with AXON_EMBEDDING_DEVICE=cpu for the gate; report GPU separately.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path, help="indexed copy of the 78-file Godot runtime")
    parser.add_argument("--axon", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--summary", type=Path, help="write compact, versionable evidence JSON")
    args = parser.parse_args()
    manifest = json.loads((Path(__file__).parent / "retrieval-v1.json").read_text())
    results = {}
    env = dict(os.environ, AXON_EMBEDDING_DEVICE="cpu")
    for mode in ("semantic", "hybrid"):
        rows = []
        for case in manifest["questions"]:
            start = time.perf_counter()
            run = subprocess.run(
                [str(args.axon), "capsule", case["query"], "--no-cache",
                 f"--retrieval-mode={mode}"], cwd=args.project, env=env,
                text=True, capture_output=True, check=True,
            )
            elapsed = (time.perf_counter() - start) * 1000
            capsule = json.loads(run.stdout)
            by_path = {f["path"]: f["content"] for f in
                       capsule["pivot_files"] + capsule["support_files"]}
            supported = all(
                path in by_path and all(marker in by_path[path] for marker in markers)
                for path, markers in case["required"].items()
            )
            rows.append({"id": case["id"], "kind": case["kind"],
                         "supported_proxy": supported,
                         "token_estimate": capsule["token_estimate"],
                         "latency_ms": round(elapsed, 2),
                         "pivot_paths": [f["path"] for f in capsule["pivot_files"]],
                         "selection": capsule["selection"]})
        results[mode] = {
            "rows": rows,
            "support_rate": sum(r["supported_proxy"] for r in rows) / len(rows),
            "original_support": sum(r["supported_proxy"] for r in rows if r["kind"] == "original"),
            "exact_support": sum(r["supported_proxy"] for r in rows if r["kind"] == "exact"),
            "latency_p95_ms": round(percentile([r["latency_ms"] for r in rows], .95), 2),
            "max_tokens": max(r["token_estimate"] for r in rows),
        }
    baseline, candidate = results["semantic"], results["hybrid"]
    results["gate"] = {
        "original_3_of_4": candidate["original_support"] >= 3,
        "full_80_percent": candidate["support_rate"] >= .8,
        "exact_no_regression": candidate["exact_support"] >= baseline["exact_support"],
        "budget_8000": candidate["max_tokens"] <= 8000,
        "p95_at_most_1_5x": candidate["latency_p95_ms"] <= baseline["latency_p95_ms"] * 1.5,
    }
    output = json.dumps(results, indent=2, ensure_ascii=False)
    if args.output:
        args.output.write_text(output + "\n")
    else:
        print(output)
    if args.summary:
        summary = {"evaluation": "evals/godot/retrieval-v1.json", "device": "cpu",
                   "gate": results["gate"], "modes": {}}
        for mode in ("semantic", "hybrid"):
            value = results[mode]
            summary["modes"][mode] = {
                key: value[key] for key in
                ("support_rate", "original_support", "exact_support", "latency_p95_ms", "max_tokens")
            }
            summary["modes"][mode]["cases"] = [
                {key: row[key] for key in
                 ("id", "kind", "supported_proxy", "token_estimate", "latency_ms", "pivot_paths")}
                for row in value["rows"]
            ]
        args.summary.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    return 0 if all(results["gate"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())
