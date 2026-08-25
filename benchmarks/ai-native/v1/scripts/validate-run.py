#!/usr/bin/env python3
"""Deterministically validates one benchmark trial's KAI source.

Usage:
    validate-run.py <task-number: 01|02|03> <path-to-benchmark.kai>
                     [--kaicc PATH] [--write-result PATH]

Steps performed (per benchmarks/ai-native/v1/README.md's validation
protocol):
    1. invoke the current kaicc binary to compile the given source
    2. compile to a temporary output executable
    3. run it
    4. capture stdout
    5. compare exact output against benchmarks/ai-native/v1/expected/task-<NN>.stdout
    6. report PASS/FAIL

This script only checks objective correctness (compiles, runs, exact
stdout match). It does NOT measure or infer agent behavior (tool calls,
elapsed time, token usage) - those are recorded separately by whoever runs
the agent trial, per the README's measurement protocol.
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def find_repo_root(start: Path) -> Path:
    current = start.resolve()
    for candidate in [current, *current.parents]:
        if (candidate / "CMakeLists.txt").is_file() and (candidate / "benchmarks").is_dir():
            return candidate
    raise SystemExit("error: could not locate repository root (no CMakeLists.txt + benchmarks/ found)")


def default_kaicc_path(repo_root: Path) -> Path:
    return repo_root / "build" / "bin" / "kaicc"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("task_number", choices=["01", "02", "03"], help="which task's expected output to check against")
    parser.add_argument("source_path", type=Path, help="path to the benchmark.kai file to validate")
    parser.add_argument("--kaicc", type=Path, default=None, help="path to the kaicc binary (default: <repo>/build/bin/kaicc)")
    parser.add_argument("--write-result", type=Path, default=None, help="optional path to write a result.json stub with the validation outcome")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    bench_root = script_dir.parent
    repo_root = find_repo_root(bench_root)

    kaicc_path = args.kaicc if args.kaicc is not None else default_kaicc_path(repo_root)
    if not kaicc_path.is_file():
        print(f"error: kaicc binary not found at {kaicc_path} (build it first: cmake --build build)", file=sys.stderr)
        return 2

    source_path = args.source_path.resolve()
    if not source_path.is_file():
        print(f"error: source file not found: {source_path}", file=sys.stderr)
        return 2

    expected_path = bench_root / "expected" / f"task-{args.task_number}.stdout"
    if not expected_path.is_file():
        print(f"error: expected output not found: {expected_path}", file=sys.stderr)
        return 2
    expected_stdout = expected_path.read_bytes()

    with tempfile.TemporaryDirectory(prefix="kai_bench_validate_") as tmp_dir:
        output_path = Path(tmp_dir) / "benchmark_out"

        compile_result = subprocess.run(
            [str(kaicc_path), str(source_path), "-o", str(output_path)],
            capture_output=True,
            text=True,
        )
        compile_succeeded = compile_result.returncode == 0

        run_exit_code = None
        actual_stdout = b""
        if compile_succeeded:
            run_result = subprocess.run([str(output_path)], capture_output=True)
            run_exit_code = run_result.returncode
            actual_stdout = run_result.stdout

        run_succeeded = compile_succeeded and run_exit_code == 0
        stdout_matches = compile_succeeded and actual_stdout == expected_stdout
        validation_pass = compile_succeeded and run_succeeded and stdout_matches

        print(f"task: task-{args.task_number}")
        print(f"source: {source_path}")
        print(f"compile succeeded: {compile_succeeded}")
        if not compile_succeeded:
            print("--- kaicc stderr ---")
            print(compile_result.stderr, end="")
        else:
            print(f"run exit code: {run_exit_code}")
            print(f"stdout matches expected: {stdout_matches}")
            if not stdout_matches:
                print("--- expected stdout ---")
                sys.stdout.buffer.write(expected_stdout)
                print("--- actual stdout ---")
                sys.stdout.buffer.write(actual_stdout)

        print()
        print("RESULT: PASS" if validation_pass else "RESULT: FAIL")

        if args.write_result is not None:
            result = {
                "schemaVersion": 1,
                "benchmark": "kai-ai-native-v1",
                "task": f"task-{args.task_number}",
                "validationPass": validation_pass,
                "compileSucceeded": compile_succeeded,
                "runExitCode": run_exit_code,
            }
            args.write_result.write_text(json.dumps(result, indent=2) + "\n")
            print(f"wrote validation stub to {args.write_result}")

        return 0 if validation_pass else 1


if __name__ == "__main__":
    sys.exit(main())
