#!/usr/bin/env python3
"""Run the compiled deterministic portfolio candidate evaluator and print its JSON report."""
from __future__ import annotations
import argparse
import json
import pathlib
import subprocess

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = subprocess.run([str(args.evaluator.resolve())], text=True, capture_output=True)
    if result.returncode:
        raise SystemExit(result.stderr or result.stdout)
    report = json.loads(result.stdout)
    assert report["schema_version"] == "axon/portfolio-eval/v1"
    assert report["precision_at_1"] == {"name_only": 0, "semantic_only": 0, "multi_signal": 1}
    assert report["recall_at_1"] == {"name_only": 0, "semantic_only": 0, "multi_signal": 1}
    print(json.dumps(report, sort_keys=True))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
