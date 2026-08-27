#!/usr/bin/env python3
"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: run a single deterministic
ScriptedAdapter dry-run session against an already-prepared trial.

This is infrastructure tooling, not a benchmark trial runner - it never
contacts a real model provider, and the session it produces is always
marked dryRun: true, stored under <trial>/host/agent/, and never counted
by scripts/summarize-results.py. See ISOLATION.md's "Isolation M3A"
section.

Usage:
    run-agent-dry-run.py --trial <trial-id> --compiler-root <portable-install> \\
        --fixture <textual-happy-path|semantic-happy-path|textual-illegal-semantic-call> \\
        [--root <isolated-root>]

The trial must already exist (see prepare-isolated-trial.sh).
"""

import argparse
import json
import os
import sys

_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _SCRIPTS_DIR)

from agent import fixtures  # noqa: E402
from agent.adapter import ScriptedAdapter  # noqa: E402
from agent.orchestrator import read_condition, run_session  # noqa: E402
from isolation.safe_source import read_validated_source  # noqa: E402


def build_fixture(name, initial_source):
    if name == "textual-happy-path":
        return fixtures.textual_happy_path(initial_source)
    if name == "semantic-happy-path":
        return fixtures.semantic_happy_path(initial_source)
    if name == "textual-illegal-semantic-call":
        return fixtures.textual_illegal_semantic_call()
    raise SystemExit(f"error: unknown fixture: {name!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trial", required=True)
    parser.add_argument("--compiler-root", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--root", default=os.environ.get("KAI_BENCH_ISOLATED_ROOT", "/tmp/kai-ai-native-v1/isolated"))
    args = parser.parse_args()

    trial_root = os.path.join(args.root, args.trial)
    if not os.path.isdir(trial_root):
        raise SystemExit(f"error: no prepared trial found at {trial_root}")

    workspace_dir = os.path.join(trial_root, "workspace")
    initial_source = read_validated_source(workspace_dir).decode("utf-8", errors="replace")
    script = build_fixture(args.fixture, initial_source)
    adapter = ScriptedAdapter(script)

    session, metrics, dry_run_validation = run_session(trial_root, args.compiler_root, adapter, root=args.root)

    print(json.dumps({"session": session, "metrics": metrics, "dryRunValidation": dry_run_validation}, indent=2))


if __name__ == "__main__":
    main()
