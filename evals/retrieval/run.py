#!/usr/bin/env python3
"""Measure whether a capsule delivers the gold symbols for held-out questions.

For every question the runner asks `axon capsule` (cache off, CPU embeddings) and reports:
  body       gold symbol delivered with its body ("// === name (kind) lines a-b ===")
  signature  gold symbol delivered at least as a signature line
  file       every gold file appears in the capsule
and the mean capsule size in estimated tokens (bytes/4, as Axon counts them).

Usage:
  evals/retrieval/run.py --axon build/axon --corpus /path/to/indexed/corpus \
      --questions evals/retrieval/questions/cpp.json [--mode dense] [--min-body-rate 0.7]

The corpus must be indexed with the model under test (`axon index` inside it) and contain a
project marker (`git init` is enough). See README.md for how each corpus is built.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--axon", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--questions", required=True)
    parser.add_argument("--mode", default="dense", choices=["dense", "semantic", "hybrid"])
    parser.add_argument("--min-body-rate", type=float, default=0.0)
    parser.add_argument("--json", help="write per-question results here")
    args = parser.parse_args()

    questions = [q for q in json.load(open(args.questions)) if q["gold"]]
    env = dict(os.environ, AXON_EMBEDDING_DEVICE=os.environ.get("AXON_EMBEDDING_DEVICE", "cpu"))
    rows = []
    for q in questions:
        run = subprocess.run(
            [args.axon, "capsule", q["query"], "--no-cache", f"--retrieval-mode={args.mode}"],
            cwd=args.corpus, env=env, capture_output=True, text=True)
        try:
            capsule = json.loads(run.stdout)
        except ValueError:
            rows.append({"id": q["id"], "body": False, "signature": False, "file": False, "tokens": 0,
                         "error": run.stderr[-200:]})
            continue
        by_path = {}
        for entry in capsule["pivot_files"] + capsule["support_files"]:
            by_path[entry["path"]] = by_path.get(entry["path"], "") + entry["content"]

        def body(g):
            return g["file"] in by_path and re.search(
                r"// === " + re.escape(g["symbol"]) + r" \(", by_path[g["file"]])

        def signature(g):
            return g["file"] in by_path and re.search(
                r"(^|\n)(// )?" + re.escape(g["symbol"]) + r" \(\w+\) lines", by_path[g["file"]])

        rows.append({
            "id": q["id"],
            "body": all(body(g) for g in q["gold"]),
            "signature": all(body(g) or signature(g) for g in q["gold"]),
            "file": all(g["file"] in by_path for g in q["gold"]),
            "tokens": capsule["token_estimate"],
        })
    n = len(rows)
    body_rate = sum(r["body"] for r in rows) / n
    print(f"{os.path.basename(args.questions)} mode={args.mode} n={n} "
          f"body {sum(r['body'] for r in rows)}/{n} ({body_rate:.0%}) "
          f"signature {sum(r['signature'] for r in rows)}/{n} "
          f"file {sum(r['file'] for r in rows)}/{n} "
          f"tokens {statistics.mean(r['tokens'] for r in rows):.0f}")
    if args.json:
        json.dump(rows, open(args.json, "w"), indent=1)
    return 0 if body_rate >= args.min_body_rate else 1


if __name__ == "__main__":
    sys.exit(main())
