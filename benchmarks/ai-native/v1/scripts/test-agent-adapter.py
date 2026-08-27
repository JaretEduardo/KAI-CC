#!/usr/bin/env python3
"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: automated tests proving the
trusted orchestrator's dispatch, validation, prompt-equality, resource
limits, and defense-in-depth actually work - never fakes success by only
inspecting orchestrator.py's source.

None of these sessions are formal benchmark trials: every one uses
ScriptedAdapter (a deterministic, offline fixture player - see
agent/adapter.py) and is marked dryRun: true. Formal trial counts remain
unaffected (textual = 0, semantic = 0).

Requires: podman or docker, python3, and a portable KAI-CC release tree
(default: dist/kai-linux-x86_64 relative to the repo root; override with
--compiler-root).

Usage:
    test-agent-adapter.py [--compiler-root DIR]

Exits 0 only if every check passes. Always cleans up the trials/sockets
it creates, even on failure.
"""

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(SCRIPT_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(BENCH_ROOT)))
sys.path.insert(0, SCRIPT_DIR)

from agent import fixtures  # noqa: E402
from agent.adapter import ScriptedAdapter  # noqa: E402
from agent.orchestrator import ToolContext, dispatch_tool, run_session  # noqa: E402
from agent.prompts import BASELINE_TOOL_SCHEMAS, build_tool_schemas, common_baseline_tools_sha256, sha256_of_json  # noqa: E402
from isolation.safe_source import read_validated_source  # noqa: E402

PASS_COUNT = 0
FAIL_COUNT = 0


def check(description, condition):
    global PASS_COUNT, FAIL_COUNT
    if condition:
        print(f"PASS: {description}")
        PASS_COUNT += 1
    else:
        print(f"FAIL: {description}")
        FAIL_COUNT += 1


def prepare_trial(bench_root, task, condition, isolated_root):
    prepare_script = os.path.join(bench_root, "scripts", "prepare-isolated-trial.sh")
    env = dict(os.environ)
    env["KAI_BENCH_ISOLATED_ROOT"] = isolated_root
    result = subprocess.run(
        [prepare_script, "--task", task, "--condition", condition],
        capture_output=True,
        text=True,
        env=env,
    )
    if result.returncode != 0:
        raise RuntimeError(f"prepare-isolated-trial.sh failed: {result.stderr}")
    trial_id = None
    for line in result.stdout.splitlines():
        if line.startswith("Prepared isolated trial: "):
            trial_id = line[len("Prepared isolated trial: "):].strip()
    if not trial_id:
        raise RuntimeError(f"could not parse trial id from: {result.stdout}")
    return trial_id, os.path.join(isolated_root, trial_id)


def sandbox_exec(bench_root, trial_id, isolated_root, *cmd, timeout=30):
    script = os.path.join(bench_root, "scripts", "sandbox-exec.sh")
    result = subprocess.run(
        [script, trial_id, "--root", isolated_root, "--"] + list(cmd),
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler-root", default=os.path.join(REPO_ROOT, "dist", "kai-linux-x86_64"))
    args = parser.parse_args()

    if not os.path.isfile(os.path.join(args.compiler_root, "bin", "kaicc")):
        print(f"error: no portable compiler found at {args.compiler_root} (bin/kaicc missing).", file=sys.stderr)
        print("       Build one with scripts/build-release-linux-x86_64.sh, or pass --compiler-root.", file=sys.stderr)
        return 1

    test_root = tempfile.mkdtemp(prefix="kai-ai-native-v1-agent-adapter-test.")
    isolated_root = os.path.join(test_root, "isolated")
    toolchains_root = os.path.join(test_root, "toolchains")

    try:
        run_all_tests(args.compiler_root, isolated_root, toolchains_root)
    finally:
        shutil.rmtree(test_root, ignore_errors=True)
        # Also remove any sockets this run created directly under the
        # shared /tmp/kai-ai-native-v1/sockets/ location (not test_root -
        # sockets deliberately live at a short, flat path; see
        # ISOLATION.md's Isolation M2 section on AF_UNIX path length).
        sockets_dir = "/tmp/kai-ai-native-v1/sockets"
        if os.path.isdir(sockets_dir):
            for name in os.listdir(sockets_dir):
                if name.startswith("task-0"):
                    try:
                        os.unlink(os.path.join(sockets_dir, name))
                    except OSError:
                        pass

    print()
    print("=== Summary ===")
    print(f"{PASS_COUNT} passed, {FAIL_COUNT} failed")
    return 0 if FAIL_COUNT == 0 else 1


def run_all_tests(compiler_root, isolated_root, toolchains_root):
    def run(trial_root, script, limits=None):
        adapter = ScriptedAdapter(script)
        return run_session(trial_root, compiler_root, adapter, limits=limits, root=isolated_root, toolchains_root=toolchains_root)

    print("=== Scripted textual dry-run (item 24) ===")
    tid, troot = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    initial_source = read_validated_source(os.path.join(troot, "workspace")).decode("utf-8")
    session, metrics, dry_run = run(troot, fixtures.textual_happy_path(initial_source))
    check("textual dry-run terminates via finish_tool", session["terminationReason"] == "finish_tool")
    check("textual dry-run is marked dryRun: true", session["dryRun"] is True)
    check("textual dry-run adapterType is 'scripted-v1', never a vendor name", session["adapterType"] == "scripted-v1")
    check(
        "textual dry-run exercised read/replace/compile/run/finish",
        set(metrics["toolCallsByOperation"].keys()) == {"read_source", "replace_source", "compile", "run", "finish"},
    )
    check("textual dry-run recorded a real compiler version", session["compilerVersion"] not in (None, "", "unknown"))
    check("textual dry-run recorded a real compiler sha256", session["compilerSha256"].startswith("sha256:"))
    check("textual dry-run produced a dry-run validation diagnostic (host-only)", dry_run is not None)

    print()
    print("=== Scripted semantic dry-run (item 25) ===")
    tid2, troot2 = prepare_trial(BENCH_ROOT, "01", "semantic", isolated_root)
    initial_source2 = read_validated_source(os.path.join(troot2, "workspace")).decode("utf-8")
    session2, metrics2, _ = run(troot2, fixtures.semantic_happy_path(initial_source2))
    check("semantic dry-run terminates via finish_tool", session2["terminationReason"] == "finish_tool")
    check("semantic dry-run exercised a real semantic query", metrics2["semanticQueryCalls"] == 1)
    check("semantic dry-run also compiled and ran", metrics2["compileCalls"] == 1 and metrics2["runCalls"] == 1)

    print()
    print("=== Textual illegal semantic-call test (item 26) ===")
    tid3, troot3 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session3, metrics3, _ = run(troot3, fixtures.textual_illegal_semantic_call())
    with open(os.path.join(troot3, "host", "agent", "transcript.jsonl")) as f:
        transcript3 = [json.loads(line) for line in f]
    inspect_result = [e for e in transcript3 if e.get("event") == "tool_result" and e.get("tool") == "inspect"]
    check("orchestrator denies a textual trial's 'inspect' call", len(inspect_result) == 1 and inspect_result[0]["allowed"] is False)
    check("session still terminates cleanly (finish) after the denial", session3["terminationReason"] == "finish_tool")

    print()
    print("=== Prompt-pair equality, all three tasks (items 7, 27) ===")
    for task in ("01", "02", "03"):
        t_id, t_root = prepare_trial(BENCH_ROOT, task, "textual", isolated_root)
        s_id, s_root = prepare_trial(BENCH_ROOT, task, "semantic", isolated_root)
        t_session, _, _ = run(t_root, [fixtures.response("Just finishing.", [fixtures.call("c1", "finish")])])
        s_session, _, _ = run(s_root, [fixtures.response("Just finishing.", [fixtures.call("c1", "finish")])])
        check(f"task-{task}: system prompt hash identical across conditions", t_session["systemPromptSha256"] == s_session["systemPromptSha256"])
        check(f"task-{task}: user prompt hash identical across conditions", t_session["userPromptSha256"] == s_session["userPromptSha256"])
        check(f"task-{task}: common baseline tool schema hash identical across conditions", t_session["commonToolsSha256"] == s_session["commonToolsSha256"])
        with open(os.path.join(t_root, "workspace", "trial.json")) as f:
            t_hashes = json.load(f)["inputHashes"]
        with open(os.path.join(s_root, "workspace", "trial.json")) as f:
            s_hashes = json.load(f)["inputHashes"]
        check(f"task-{task}: starting source/TASK.md hashes identical across conditions", t_hashes == s_hashes)

    print()
    print("=== Tool-surface equality (item 28) ===")
    textual_tools = build_tool_schemas("textual")
    semantic_tools = build_tool_schemas("semantic")
    check("textual advertises exactly the 5 baseline tools", [t["name"] for t in textual_tools] == [t["name"] for t in BASELINE_TOOL_SCHEMAS])
    check(
        "semantic advertises the SAME 5 baseline tools first, byte-for-byte",
        sha256_of_json(semantic_tools[: len(BASELINE_TOOL_SCHEMAS)]) == sha256_of_json(textual_tools),
    )
    check(
        "semantic additionally advertises exactly the 6 semantic query tools",
        {t["name"] for t in semantic_tools[len(BASELINE_TOOL_SCHEMAS):]}
        == {"inspect", "definition", "references", "callers", "callees", "call-graph"},
    )
    check(
        "common_baseline_tools_sha256() agrees for both conditions",
        common_baseline_tools_sha256("textual") == common_baseline_tools_sha256("semantic"),
    )

    print()
    print("=== Path-injection / unexpected-argument tests (item 29) ===")
    tid4, troot4 = prepare_trial(BENCH_ROOT, "02", "textual", isolated_root)
    injection_cases = [
        ("read_source", {"path": "/etc/passwd"}),
        ("read_source", {"filename": "../reference/task-01.kai"}),
        ("replace_source", {"content": "fn main() {}", "trialId": "some-other-trial"}),
        ("replace_source", {"content": "fn main() {}", "condition": "semantic"}),
    ]
    script4 = [fixtures.response("Probing.", [fixtures.call(f"inj{i}", name, args)]) for i, (name, args) in enumerate(injection_cases)]
    script4.append(fixtures.response("Done.", [fixtures.call("finish", "finish")]))
    _, _, _ = run(troot4, script4)
    with open(os.path.join(troot4, "host", "agent", "transcript.jsonl")) as f:
        transcript4 = [json.loads(line) for line in f]
    denied_count = sum(1 for e in transcript4 if e.get("event") == "tool_result" and e.get("tool") in ("read_source", "replace_source") and e.get("allowed") is False)
    check("all 4 path/trialId/condition injection attempts are rejected, not ignored", denied_count == 4)

    print()
    print("=== Source symlink/FIFO attacks against read_source/replace_source (items 30, 31) ===")
    print("    (mid-session swaps - the trial starts with a genuinely pristine")
    print("     source, exactly like a real prepared trial; the attack replaces")
    print("     benchmark.kai BETWEEN two tool calls within the same session,")
    print("     matching the live-tampering threat model, not a pre-broken trial)")

    class SwapSourceAdapter(ScriptedAdapter):
        """Performs a filesystem swap as a side effect immediately before
        a specific scripted turn - simulates an agent/attacker replacing
        benchmark.kai WHILE the session is live, in between two ordinary
        tool calls, rather than before the session even starts."""

        def __init__(self, script, swap_before_turn, swap_fn):
            super().__init__(script)
            self._swap_before_turn = swap_before_turn
            self._swap_fn = swap_fn
            self._turn = 0

        def next_response(self, messages, tools):
            self._turn += 1
            if self._turn == self._swap_before_turn:
                self._swap_fn()
            return super().next_response(messages, tools)

    def run_swap(trial_root, script, swap_before_turn, swap_fn):
        adapter = SwapSourceAdapter(script, swap_before_turn, swap_fn)
        return run_session(trial_root, compiler_root, adapter, root=isolated_root, toolchains_root=toolchains_root)

    tid5, troot5 = prepare_trial(BENCH_ROOT, "03", "textual", isolated_root)
    ws5 = os.path.join(troot5, "workspace")
    secret_fd, secret_path = tempfile.mkstemp(prefix="kai-m3a-secret.")
    os.write(secret_fd, b"THIS-MUST-NEVER-BE-READ-OR-COMPILED")
    os.close(secret_fd)

    def swap_to_absolute_symlink():
        os.remove(os.path.join(ws5, "benchmark.kai"))
        os.symlink(secret_path, os.path.join(ws5, "benchmark.kai"))

    session5, _, _ = run_swap(
        troot5,
        [
            fixtures.response("Reading (before the swap).", [fixtures.call("r0", "read_source")]),
            fixtures.response("Reading again (after the swap).", [fixtures.call("r1", "read_source")]),
            fixtures.response("Replacing.", [fixtures.call("r2", "replace_source", {"content": "fn main() {}"})]),
            fixtures.response("Done.", [fixtures.call("r3", "finish")]),
        ],
        swap_before_turn=2,
        swap_fn=swap_to_absolute_symlink,
    )
    with open(os.path.join(troot5, "host", "agent", "transcript.jsonl")) as f:
        transcript5 = [json.loads(line) for line in f]
    read_results5 = [e for e in transcript5 if e.get("event") == "tool_result" and e.get("tool") == "read_source"]
    write_denied = next(e for e in transcript5 if e.get("event") == "tool_result" and e.get("tool") == "replace_source")
    check("read_source succeeds before the mid-session swap", read_results5[0]["allowed"] is True)
    check("read_source refuses an absolute symlink to a real host file after the swap", read_results5[1]["allowed"] is False)
    check("replace_source refuses to write through the same absolute symlink", write_denied["allowed"] is False)
    check("session still terminates cleanly (finish) after both refusals", session5["terminationReason"] == "finish_tool")
    with open(secret_path) as f:
        check("secret host file content is unchanged after the attack", f.read() == "THIS-MUST-NEVER-BE-READ-OR-COMPILED")
    os.remove(secret_path)

    tid6, troot6 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    ws6 = os.path.join(troot6, "workspace")

    def swap_to_relative_escaping_symlink():
        os.remove(os.path.join(ws6, "benchmark.kai"))
        os.symlink("../../../../etc/passwd", os.path.join(ws6, "benchmark.kai"))

    run_swap(
        troot6,
        [
            fixtures.response("Reading (before the swap).", [fixtures.call("r0", "read_source")]),
            fixtures.response("Reading again (after the swap).", [fixtures.call("r1", "read_source")]),
            fixtures.response("Done.", [fixtures.call("r2", "finish")]),
        ],
        swap_before_turn=2,
        swap_fn=swap_to_relative_escaping_symlink,
    )
    with open(os.path.join(troot6, "host", "agent", "transcript.jsonl")) as f:
        transcript6 = [json.loads(line) for line in f]
    read_results6 = [e for e in transcript6 if e.get("event") == "tool_result" and e.get("tool") == "read_source"]
    check("read_source refuses a relative symlink escaping the workspace after the swap", read_results6[1]["allowed"] is False)

    tid7, troot7 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    ws7 = os.path.join(troot7, "workspace")

    def swap_to_fifo():
        os.remove(os.path.join(ws7, "benchmark.kai"))
        os.mkfifo(os.path.join(ws7, "benchmark.kai"))

    started = __import__("time").monotonic()
    run_swap(
        troot7,
        [
            fixtures.response("Reading (before the swap).", [fixtures.call("r0", "read_source")]),
            fixtures.response("Reading again (after the swap).", [fixtures.call("r1", "read_source")]),
            fixtures.response("Done.", [fixtures.call("r2", "finish")]),
        ],
        swap_before_turn=2,
        swap_fn=swap_to_fifo,
    )
    elapsed = __import__("time").monotonic() - started
    with open(os.path.join(troot7, "host", "agent", "transcript.jsonl")) as f:
        transcript7 = [json.loads(line) for line in f]
    read_results7 = [e for e in transcript7 if e.get("event") == "tool_result" and e.get("tool") == "read_source"]
    check("read_source refuses a FIFO immediately after the swap, not by blocking", read_results7[1]["allowed"] is False and elapsed < 15)

    print()
    print("=== Run-tool safety (item 32) ===")
    tid8, troot8 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session8, metrics8, _ = run(
        troot8,
        [
            fixtures.response("Running before compiling.", [fixtures.call("run1", "run")]),
            fixtures.response("Done.", [fixtures.call("f1", "finish")]),
        ],
    )
    with open(os.path.join(troot8, "host", "agent", "transcript.jsonl")) as f:
        transcript8 = [json.loads(line) for line in f]
    run_before_compile = next(e for e in transcript8 if e.get("event") == "tool_result" and e.get("tool") == "run")
    check("run refuses when benchmark_out is absent", run_before_compile["allowed"] is False)

    tid9, troot9 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session9, metrics9, _ = run(troot9, fixtures.textual_happy_path(read_validated_source(os.path.join(troot9, "workspace")).decode("utf-8")))
    sandboxed_run = sandbox_exec(BENCH_ROOT, tid9, isolated_root, "test", "-e", "/workspace/benchmark_out")
    check("run's fixed executable target (benchmark_out) exists after compile", sandboxed_run.returncode == 0)

    print()
    print("=== Host agent metadata invisibility inside the sandbox (item 33) ===")
    host_hidden_check = sandbox_exec(BENCH_ROOT, tid9, isolated_root, "test", "!", "-e", "/workspace/host")
    check("host/ is not visible as /workspace/host inside the sandbox", host_hidden_check.returncode == 0)

    # find / -xdev legitimately exits nonzero on this hardened, non-root,
    # read-only sandbox because IT CANNOT READ some ordinary unrelated
    # system directories (/root, /etc/ssl/private, etc.) - that permission
    # denial is itself evidence of correct sandboxing, not a test failure.
    # What matters is that our two target filenames never appear in stdout
    # (stderr, carrying the unrelated permission-denied noise, is
    # discarded by the command's own "2>/dev/null").
    find_check = sandbox_exec(
        BENCH_ROOT, tid9, isolated_root, "bash", "-c",
        "find / -xdev -iname 'session.json' -o -iname 'transcript.jsonl' 2>/dev/null",
    )
    check("host/agent/session.json and transcript.jsonl are not found anywhere inside the sandbox", find_check.stdout.strip() == "")

    print()
    print("=== Finish/collection boundary (item 34) ===")
    collected_path = os.path.join(troot9, "host", "result", "benchmark.kai")
    final_workspace_path = os.path.join(troot9, "workspace", "benchmark.kai")
    check("collected source exists after finish", os.path.isfile(collected_path))
    with open(collected_path, "rb") as f:
        collected_bytes = f.read()
    with open(final_workspace_path, "rb") as f:
        final_bytes = f.read()
    check("collected source matches the final workspace source exactly", collected_bytes == final_bytes)
    result_dir_files = sorted(os.listdir(os.path.join(troot9, "host", "result")))
    check("only benchmark.kai + collection.json crossed the collection boundary", result_dir_files == ["benchmark.kai", "collection.json"])

    print()
    print("=== Adapter/response failure handling (item 35) ===")
    tid10, troot10 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session10, _, _ = run(troot10, fixtures.malformed_bad_shape())
    check("a malformed adapter response terminates with 'malformed_response'", session10["terminationReason"] == "malformed_response")

    tid11, troot11 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session11, _, _ = run(troot11, fixtures.malformed_unknown_tool())
    with open(os.path.join(troot11, "host", "agent", "transcript.jsonl")) as f:
        transcript11 = [json.loads(line) for line in f]
    unknown_result = next(e for e in transcript11 if e.get("event") == "tool_result" and e.get("tool") == "delete_everything")
    check("an unknown tool name is denied, never executed", unknown_result["allowed"] is False)
    check("session terminates cleanly (not a crash) after an unknown tool", session11["terminationReason"] in ("end_turn_no_tool_calls", "no_tool_calls"))

    tid12, troot12 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session12, metrics12, _ = run(troot12, fixtures.tool_call_limit_probe(50), limits={"max_tool_calls": 5})
    check("exceeding max_tool_calls terminates with 'max_tool_calls_exceeded'", session12["terminationReason"] == "max_tool_calls_exceeded")
    check("tool-call-limit session does not pretend success", session12["terminationReason"] != "finish_tool")

    tid13, troot13 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    session13, metrics13, _ = run(troot13, fixtures.turn_limit_probe(30), limits={"max_turns": 5})
    check("exceeding max_turns terminates with 'max_turns_exceeded'", session13["terminationReason"] == "max_turns_exceeded")

    print()
    print("=== Crash/exception cleanup (item 36) ===")

    class ExplodingAdapter(ScriptedAdapter):
        def next_response(self, messages, tools):
            raise RuntimeError("simulated adapter crash")

    tid14, troot14 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    adapter14 = ExplodingAdapter([])
    session14, _, _ = run_session(troot14, compiler_root, adapter14, root=isolated_root, toolchains_root=toolchains_root)
    check("an adapter exception terminates with 'adapter_error', never crashes the orchestrator", session14["terminationReason"] == "adapter_error")
    sock_path14 = f"/tmp/kai-ai-native-v1/sockets/{tid14}.sock"
    check("broker socket is removed after an adapter crash", not os.path.exists(sock_path14))
    ps = subprocess.run(["pgrep", "-f", f"broker.py.*{tid14}"], capture_output=True, text=True)
    check("no stale broker process remains after an adapter crash", ps.returncode != 0)

    print()
    print("=== Max source size - real orchestrator-level test (review round 3, item 1) ===")
    tid15, troot15 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    ws15 = os.path.join(troot15, "workspace")
    original_bytes15 = read_validated_source(ws15)
    original_sha15 = hashlib.sha256(original_bytes15).hexdigest()
    oversized_content = "x" * 500  # well over the tiny 128-byte test limit below
    session15, metrics15, _ = run(
        troot15,
        [
            fixtures.response("Attempting an oversized replace_source.", [fixtures.call("c1", "replace_source", {"content": oversized_content})]),
            fixtures.response("Ending.", [fixtures.call("c2", "finish")]),
        ],
        limits={"max_source_bytes": 128},
    )
    with open(os.path.join(troot15, "host", "agent", "transcript.jsonl")) as f:
        transcript15 = [json.loads(line) for line in f]
    replace_result15 = next(e for e in transcript15 if e.get("event") == "tool_result" and e.get("tool") == "replace_source")
    check("oversized replace_source is rejected (allowed: false)", replace_result15["allowed"] is False)
    current_bytes15 = read_validated_source(ws15)
    check("benchmark.kai is byte-identical to the pristine original after rejection", current_bytes15 == original_bytes15)
    check("benchmark.kai's SHA-256 is unchanged after rejection", hashlib.sha256(current_bytes15).hexdigest() == original_sha15)
    check("no truncated/partial source was written (exact original length preserved)", len(current_bytes15) == len(original_bytes15))
    check("session continues normally per existing tool-error behavior (still reaches finish)", session15["terminationReason"] == "finish_tool")
    check("rejection did not cross into any formal benchmark result location", not os.path.exists(os.path.join(BENCH_ROOT, "runs", tid15)))

    # Transcript hygiene: the oversized attempted content must never be
    # serialized into the host-only transcript, in either the tool_call
    # (args) or tool_result event for this call.
    call_event15 = next(e for e in transcript15 if e.get("event") == "tool_call" and e.get("tool") == "replace_source")
    transcript_line_text15 = json.dumps(call_event15)
    check("transcript's tool_call event does not contain the oversized payload text", oversized_content not in transcript_line_text15)
    check("transcript's tool_call event logs only a byte count for replace_source content", call_event15["argsNormalized"] == {"contentBytes": len(oversized_content.encode("utf-8"))})
    result_line_text15 = json.dumps(replace_result15)
    check("transcript's tool_result event does not contain the oversized payload text either", oversized_content not in result_line_text15)

    # Same hygiene check for a SUCCESSFUL large-but-within-limit write -
    # the transcript must not echo full content even when the write
    # actually succeeds, not just on rejection.
    tid15b, troot15b = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    ws15b = os.path.join(troot15b, "workspace")
    large_ok_content = "// " + ("y" * 900) + "\nfn main() {}\n"
    run(
        troot15b,
        [
            fixtures.response("A large but within-limit replace_source.", [fixtures.call("c1", "replace_source", {"content": large_ok_content})]),
            fixtures.response("Ending.", [fixtures.call("c2", "finish")]),
        ],
        limits={"max_source_bytes": 1_000_000},
    )
    with open(os.path.join(troot15b, "host", "agent", "transcript.jsonl")) as f:
        transcript15b = [json.loads(line) for line in f]
    call_event15b = next(e for e in transcript15b if e.get("event") == "tool_call" and e.get("tool") == "replace_source")
    check("a SUCCESSFUL large replace_source also logs only a byte count, not the content", "y" * 900 not in json.dumps(call_event15b))

    print()
    print("=== Max wall time - deterministic injected-clock test (review round 3, item 2) ===")

    class ScriptedClock:
        """Returns a predetermined sequence of monotonic-style values,
        clamping to the last one once exhausted. Lets the test PROVE
        max_wall_time enforcement without any real elapsed time or
        timing-sensitive flakiness."""

        def __init__(self, values):
            self._values = list(values)
            self._index = 0

        def __call__(self):
            value = self._values[min(self._index, len(self._values) - 1)]
            self._index += 1
            return value

    tid16, troot16 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    clock16 = ScriptedClock([0.0, 0.1, 999.0, 999.0, 999.0, 999.0])
    adapter16 = ScriptedAdapter(
        [
            fixtures.response("Turn one.", [fixtures.call("c1", "read_source")]),
            fixtures.response("Turn two (should never run).", [fixtures.call("c2", "read_source")]),
        ]
    )
    session16, metrics16, _ = run_session(
        troot16, compiler_root, adapter16, limits={"max_wall_time_seconds": 1}, root=isolated_root, toolchains_root=toolchains_root, time_source=clock16
    )
    check("session terminates with an explicit 'max_wall_time_exceeded' reason", session16["terminationReason"] == "max_wall_time_exceeded")
    check("only the first turn executed before the limit was detected", metrics16["totalTurns"] == 1)
    check("no tool call from the second (never-reached) turn was recorded", metrics16["totalToolCalls"] == 1)
    with open(os.path.join(troot16, "host", "agent", "transcript.jsonl")) as f:
        transcript16 = [json.loads(line) for line in f]
    check("transcript ends cleanly with a session_end event", transcript16[-1]["event"] == "session_end" and transcript16[-1]["terminationReason"] == "max_wall_time_exceeded")
    check("collector still ran (same termination policy as other limit-exceeded cases)", os.path.isfile(os.path.join(troot16, "host", "result", "benchmark.kai")))
    sock_path16 = f"/tmp/kai-ai-native-v1/sockets/{tid16}.sock"
    check("broker socket is removed after a max-wall-time termination", not os.path.exists(sock_path16))
    ps16 = subprocess.run(["pgrep", "-f", f"broker.py.*{tid16}"], capture_output=True, text=True)
    check("no stale broker process remains after a max-wall-time termination", ps16.returncode != 0)

    print()
    print("=== Run timeout - real sandboxed integration test (review round 3, item 3) ===")
    # Verified against the real compiler first (see run-agent-dry-run.py's
    # own manual verification during development): `fn main() { while
    # true { } }` compiles and genuinely hangs when run - never assumed.
    infinite_source = "fn main() {\n    while true {\n    }\n}\n"

    tid17, troot17 = prepare_trial(BENCH_ROOT, "01", "textual", isolated_root)
    ws17 = os.path.join(troot17, "workspace")
    # Compile the non-terminating program through a full, real session
    # (never `run` yet - that part is exercised separately below via a
    # direct dispatch_tool() call, so the precise result fields
    # (timedOut, exitCode) can be inspected directly rather than only
    # through the transcript's own deliberately-bounded event shape).
    session17, metrics17, dry_run17 = run(
        troot17,
        [
            fixtures.response("Writing a non-terminating program.", [fixtures.call("c1", "replace_source", {"content": infinite_source})]),
            fixtures.response("Compiling.", [fixtures.call("c2", "compile")]),
            fixtures.response("Ending.", [fixtures.call("c3", "finish")]),
        ],
        # A short dry_run_timeout_seconds keeps THIS test fast - the
        # collected source is the infinite loop itself, so the automatic
        # post-finish host-side validation diagnostic would otherwise run
        # the full production default (20s) here too. This also doubles
        # as the regression test for the real bug discovered while
        # writing this fixture: an earlier version of
        # run_dry_run_validation() hung indefinitely (and would have
        # leaked the infinite-loop process on the host) validating a
        # non-terminating collected program - see orchestrator.py's own
        # comment on that function for the fix (bounded timeout,
        # process-group kill).
        limits={"dry_run_timeout_seconds": 3},
    )
    check("dry-run validation diagnostic reports TIMEOUT rather than hanging", dry_run17 is not None and dry_run17["resultLine"] == "TIMEOUT")
    check("the non-terminating program compiles successfully through the real orchestrator", os.path.isfile(os.path.join(ws17, "benchmark_out")))
    check("compile-only session finishes normally", session17["terminationReason"] == "finish_tool")
    sock_path17 = f"/tmp/kai-ai-native-v1/sockets/{tid17}.sock"
    check("broker socket is removed after the compile-only session", not os.path.exists(sock_path17))
    # validate-run.py compiles the collected candidate into its own
    # tempfile.TemporaryDirectory(prefix="kai_bench_validate_") and runs
    # IT - that distinctive prefix is what a leaked grandchild process
    # would show in its command line, not "benchmark.kai" itself.
    leaked17 = subprocess.run(["pgrep", "-f", "kai_bench_validate_"], capture_output=True, text=True)
    check("the dry-run-validation's own compiled candidate process does not leak on the host", leaked17.returncode != 0)

    # Now exercise the real `run` dispatch path directly, through the
    # SAME orchestrator function the full agent loop uses
    # (agent/orchestrator.dispatch_tool), against the just-compiled
    # infinite-loop executable, with a short configured timeout.
    ctx17 = ToolContext(ws17, sock_path17, tid17, isolated_root, max_source_bytes=1_000_000, run_timeout_seconds=2)
    started17 = time.monotonic()
    run_result17 = dispatch_tool("run", {}, ctx17)
    elapsed17 = time.monotonic() - started17
    check("run is allowed/attempted (the operation itself was permitted)", run_result17["allowed"] is True)
    check("the orchestrator reports a deterministic timedOut result", run_result17.get("timedOut") is True)
    check("the timed-out run's exit code reflects the container being killed (137)", run_result17.get("exitCode") == 137)
    check(
        f"run actually stopped near the configured 2s timeout, not by hanging (elapsed={elapsed17:.1f}s)",
        elapsed17 < 10,
    )
    container_check17 = subprocess.run(
        ["podman", "ps", "-a", "--filter", "ancestor=kai-bench-sandbox:m1", "--format", "{{.ID}}"], capture_output=True, text=True
    )
    check("no orphaned sandbox container remains after the timeout", container_check17.stdout.strip() == "")

    # Infrastructure cleanup after all this must still succeed cleanly -
    # remove the trial the same way cleanup-isolated-trials.sh would.
    shutil.rmtree(troot17, ignore_errors=False)
    check("subsequent host-side cleanup of the trial directory succeeds", not os.path.exists(troot17))


if __name__ == "__main__":
    sys.exit(main())
