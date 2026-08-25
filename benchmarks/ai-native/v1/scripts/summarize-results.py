#!/usr/bin/env python3
"""Summarizes result.json files produced by manual benchmark trials.

Usage:
    summarize-results.py <runs-dir>

Scans <runs-dir> recursively for files named result.json (schemaVersion 1,
see README.md), groups them by (task, condition), and prints raw values
plus simple averages for numeric metrics when more than one trial exists
per group. This is a raw-metrics summary, not a composite score - see
README.md's "Scoring" section for why a single score is deliberately not
computed here.

Standard-library only, no plotting/dependency. Does not modify or delete
any result.json file it reads.
"""

import json
import sys
from collections import defaultdict
from pathlib import Path


PRIMARY_METRICS = [
    "elapsedTime",
    "compilerInvocations",
    "failedCompilerInvocations",
    "sourceReads",
    "textualSearches",
    "semanticQueries",
]

OPTIONAL_METRICS = [
    "sourceLinesRead",
    "inputTokens",
    "outputTokens",
    "totalTokens",
]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: summarize-results.py <runs-dir>", file=sys.stderr)
        return 1

    runs_dir = Path(sys.argv[1])
    if not runs_dir.is_dir():
        print(f"error: not a directory: {runs_dir}", file=sys.stderr)
        return 1

    groups = defaultdict(list)
    for result_path in sorted(runs_dir.rglob("result.json")):
        try:
            data = json.loads(result_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print(f"warning: skipping unreadable {result_path}: {exc}", file=sys.stderr)
            continue

        if data.get("schemaVersion") != 1:
            print(f"warning: skipping {result_path}: unsupported schemaVersion {data.get('schemaVersion')!r}", file=sys.stderr)
            continue

        task = data.get("task", "unknown-task")
        condition = data.get("condition", "unknown-condition")
        groups[(task, condition)].append((result_path, data))

    if not groups:
        print(f"No result.json files found under {runs_dir}")
        return 0

    for (task, condition), entries in sorted(groups.items()):
        print(f"{task} / {condition}  ({len(entries)} trial(s))")

        pass_count = sum(1 for _, d in entries if d.get("validationPass") is True)
        print(f"  validationPass: {pass_count}/{len(entries)}")

        metrics_lists = defaultdict(list)
        for _, data in entries:
            metrics = data.get("metrics", {})
            for key in PRIMARY_METRICS + OPTIONAL_METRICS:
                value = metrics.get(key)
                if isinstance(value, (int, float)):
                    metrics_lists[key].append(value)

        for key in PRIMARY_METRICS:
            values = metrics_lists.get(key)
            if values:
                avg = sum(values) / len(values)
                print(f"  {key}: {values} (avg {avg:.2f})")

        for key in OPTIONAL_METRICS:
            values = metrics_lists.get(key)
            if values:
                avg = sum(values) / len(values)
                print(f"  {key} (optional): {values} (avg {avg:.2f})")

        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
