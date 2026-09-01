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
    parser.add_argument("--dataset", type=pathlib.Path, default=pathlib.Path(__file__).with_name("truth-set-v1.json"))
    args = parser.parse_args()
    result = subprocess.run([str(args.evaluator.resolve()), str(args.dataset.resolve())], text=True, capture_output=True)
    if result.returncode:
        raise SystemExit(result.stderr or result.stdout)
    report = json.loads(result.stdout)
    assert report["schema_version"] == "axon/portfolio-eval/v1"
    assert report["truth_cases"] == 6
    expected = {
        "name_only": (2, 4, 4),
        "semantic_only": (1, 5, 5),
        "multi_signal": (6, 0, 0),
    }
    for baseline, expected_counts in expected.items():
        metrics = report["baselines"][baseline]
        assert (metrics["tp"], metrics["fp"], metrics["fn"]) == expected_counts
        assert metrics["tp"] + metrics["fn"] == 6
        assert metrics["tp"] + metrics["fp"] == 6
        assert len(metrics["false_positives"]) == metrics["fp"]
        assert len(metrics["false_negatives"]) == metrics["fn"]
        assert 0 <= metrics["precision_at_1"] <= 1 and 0 <= metrics["recall_at_1"] <= 1
    print(json.dumps(report, sort_keys=True))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
