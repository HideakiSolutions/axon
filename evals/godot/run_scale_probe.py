#!/usr/bin/env python3
"""Compare semantic and hybrid uncached CLI cost on a copied enlarged index.

The cloned rows keep embeddings and lexical postings, but their source files do
not exist. This measures index/query scaling, not source-body rendering quality.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def measure(axon: Path, root: Path, query: str, mode: str, repeats: int):
    env = dict(os.environ, AXON_EMBEDDING_DEVICE="cpu")
    rows = []
    for _ in range(repeats):
        started = time.perf_counter()
        run = subprocess.run(["/usr/bin/time", "-f", "MAXRSS_KB=%M", str(axon),
                              "capsule", query, "--no-cache", f"--retrieval-mode={mode}"],
                             cwd=root, env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True, check=True)
        elapsed = round((time.perf_counter() - started) * 1000, 2)
        rss = int(run.stderr.rsplit("MAXRSS_KB=", 1)[1].splitlines()[0])
        rows.append({"latency_ms": elapsed, "max_rss_kb": rss})
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--axon", type=Path, required=True)
    parser.add_argument("--clone-helper", type=Path, required=True)
    parser.add_argument("--copies", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    axon = args.axon.resolve()
    helper = args.clone_helper.resolve()
    source = args.project.resolve()
    query = "Como o estado da missão é salvo e restaurado?"
    with tempfile.TemporaryDirectory(prefix="axon-lexical-scale-") as temp:
        clone = Path(temp) / "runtime-scale"
        (clone / ".axon").mkdir(parents=True)
        (clone / ".git").mkdir()
        (clone / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
        shutil.copy2(source / ".axon/index.duckdb", clone / ".axon/index.duckdb")
        subprocess.run([str(helper), str(clone / ".axon/index.duckdb"), str(args.copies)],
                       check=True)
        result = {"device": "cpu", "query": query, "copies": args.copies,
                  "repeats": args.repeats, "source_index": str(source / ".axon/index.duckdb"),
                  "cases": {}}
        for label, root in (("source", source), ("scaled", clone)):
            result["cases"][label] = {}
            for mode in ("semantic", "hybrid"):
                result["cases"][label][mode] = measure(axon, root, query, mode, args.repeats)
        output = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
        if args.output:
            args.output.write_text(output)
        else:
            print(output)


if __name__ == "__main__":
    main()
