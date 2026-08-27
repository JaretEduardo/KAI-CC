"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: trusted host orchestrator.

Runs an agent loop using a provider-neutral AgentAdapter (adapter.py),
dispatching each validated tool call to exactly one of three places:

  - direct, safe, HOST-SIDE file access (read_source/replace_source) -
    the orchestrator itself is the trusted host, so this never needs a
    sandbox round-trip, but reuses the EXACT SAME lstat+O_NOFOLLOW
    symlink/special-file defense already established for M1's collector
    and M2's broker (isolation/safe_source.py) - never reimplemented.

  - the EXISTING M2 broker over its real Unix-domain socket (compile and
    every semantic query) - the orchestrator connects as a plain socket
    client, going through the EXACT SAME enforcement path M2 already
    built and tested (condition check, strict params schema, symlink
    defense, timeouts, transcript). It never invokes kaicc directly and
    never bypasses the broker.

  - the EXISTING M1 sandboxed executor (run), via
    scripts/sandbox-exec.sh, so the compiled program executes inside the
    same network-disabled, non-root, read-only-rootfs container as every
    other sandboxed operation in this benchmark - never directly on the
    host.

The agent itself (an AgentAdapter implementation) NEVER receives a shell,
an arbitrary path, or direct sandbox/broker access - it only ever
receives back the plain dict of tool schemas defined in prompts.py and
returns structured tool-call requests, which THIS module validates and
dispatches. See ISOLATION.md's "Isolation M3A" section for the full
trusted/untrusted boundary.

This module is infrastructure only: it never contacts a real model
provider, never fabricates a benchmark result, and never writes into the
formal runs/ location summarize-results.py counts. Every session it runs
is marked dryRun: true (see write_session()) and is stored under
<trial-root>/host/agent/ - never workspace/, never the repository.
"""

import json
import os
import signal
import socket
import stat
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone

_SCRIPTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_SCRIPTS_DIR, "isolation"))
from safe_source import InvalidSource, read_validated_source, write_validated_source  # noqa: E402

from .prompts import (  # noqa: E402
    BASELINE_TOOL_SCHEMAS,
    SYSTEM_PROMPT,
    build_tool_schemas,
    build_user_prompt,
    common_baseline_tools_sha256,
    sha256_of_text,
)

SCHEMA_VERSION = 1
BENCHMARK_VERSION = "ai-native-v1"

DEFAULT_LIMITS = {
    "max_turns": 20,
    "max_tool_calls": 40,
    "max_wall_time_seconds": 120,
    "max_source_bytes": 1_000_000,
    "run_timeout_seconds": 15,
    "dry_run_timeout_seconds": 20,
}

# Every tool a model may name, and its exact accepted-argument shape -
# {} means "no arguments accepted at all". A model can NEVER select a
# path, filename, trial, condition, executable, environment, or host
# directory through any of these - none of the shapes below has a slot
# for one.
TOOL_ARG_SCHEMAS = {
    "read_source": {},
    "replace_source": {"content": str},
    "compile": {},
    "run": {},
    "finish": {},
    "inspect": {},
    "definition": {"line": int, "column": int},
    "references": {"line": int, "column": int},
    "callers": {"line": int, "column": int},
    "callees": {"line": int, "column": int},
    "call-graph": {},
}

BROKER_ROUTED_OPERATIONS = {"compile", "inspect", "definition", "references", "callers", "callees", "call-graph"}
POSITION_OPERATIONS = {"definition", "references", "callers", "callees"}


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class OrchestratorError(Exception):
    pass


def parse_adapter_response(raw):
    """Strictly validates a raw adapter response dict, regardless of
    which AgentAdapter produced it - a real provider SDK's output could
    itself be malformed, so this never trusts the shape. Raises
    ValueError with a human-readable reason on any deviation; never
    silently reinterprets a malformed response."""
    if not isinstance(raw, dict):
        raise ValueError("adapter response must be an object")

    assistant_text = raw.get("assistantText", "")
    if not isinstance(assistant_text, str):
        raise ValueError("'assistantText' must be a string")

    tool_calls_raw = raw.get("toolCalls", [])
    if not isinstance(tool_calls_raw, list):
        raise ValueError("'toolCalls' must be a list")

    tool_calls = []
    seen_ids = set()
    for entry in tool_calls_raw:
        if not isinstance(entry, dict):
            raise ValueError("each tool call must be an object")
        call_id = entry.get("id")
        name = entry.get("name")
        arguments = entry.get("arguments", {})
        if not isinstance(call_id, str) or not call_id:
            raise ValueError("tool call 'id' must be a non-empty string")
        if call_id in seen_ids:
            raise ValueError(f"duplicate tool call id: {call_id!r}")
        seen_ids.add(call_id)
        if not isinstance(name, str) or not name:
            raise ValueError("tool call 'name' must be a non-empty string")
        if not isinstance(arguments, dict):
            raise ValueError("tool call 'arguments' must be an object")
        tool_calls.append({"id": call_id, "name": name, "arguments": arguments})

    stop_reason = raw.get("stopReason", "tool_calls")
    if stop_reason not in ("tool_calls", "end_turn", "error"):
        raise ValueError(f"invalid stopReason: {stop_reason!r}")

    usage = raw.get("usage")
    if usage is not None and not isinstance(usage, dict):
        raise ValueError("'usage' must be an object or null")

    return assistant_text, tool_calls, stop_reason, usage


def validate_tool_call_args(tool_name, args):
    """Strictly validates a tool call's arguments against
    TOOL_ARG_SCHEMAS - unexpected keys, missing keys, and wrong types are
    all rejected. Never executed for a tool that fails this check."""
    if tool_name not in TOOL_ARG_SCHEMAS:
        return False, f"unknown tool: {tool_name!r}"
    schema = TOOL_ARG_SCHEMAS[tool_name]
    if not isinstance(args, dict):
        return False, "malformed tool call: arguments must be an object"

    extra = set(args.keys()) - set(schema.keys())
    if extra:
        return False, f"unexpected arguments: {sorted(extra)}"
    missing = set(schema.keys()) - set(args.keys())
    if missing:
        return False, f"missing required arguments: {sorted(missing)}"

    for key, expected_type in schema.items():
        value = args[key]
        if expected_type is int and (isinstance(value, bool) or not isinstance(value, int)):
            return False, f"argument {key!r} must be an integer"
        if expected_type is str and not isinstance(value, str):
            return False, f"argument {key!r} must be a string"
        if expected_type is int and isinstance(value, int) and not isinstance(value, bool) and value <= 0:
            return False, f"argument {key!r} must be a positive integer"

    return True, None


def normalize_args_for_log(tool_name, args):
    """Only ever emits a clean, bounded representation for the agent
    transcript - never an echo of arbitrary/oversized argument content
    (e.g. replace_source's full source text is never logged, only its
    byte length) - mirrors isolation/broker.py's own
    normalized_params()."""
    if tool_name in POSITION_OPERATIONS and isinstance(args, dict):
        line = args.get("line")
        column = args.get("column")
        if isinstance(line, int) and not isinstance(line, bool) and isinstance(column, int) and not isinstance(column, bool):
            return {"line": line, "column": column}
        return {}
    if tool_name == "replace_source" and isinstance(args, dict):
        content = args.get("content")
        if isinstance(content, str):
            return {"contentBytes": len(content.encode("utf-8"))}
        return {}
    return {}


def send_broker_request(socket_path, operation, params, timeout_seconds=35):
    """Plain Unix-socket JSON client, identical protocol to
    isolation/generate-tool-surface.sh's own _client.py - the orchestrator
    IS a trusted host process, so it connects to the broker directly
    rather than needing a sandboxed intermediary client."""
    request = {"schemaVersion": 1, "operation": operation, "params": params}
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout_seconds)
    try:
        s.connect(socket_path)
        s.sendall(json.dumps(request).encode("utf-8"))
        s.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        s.close()
    return json.loads(b"".join(chunks).decode("utf-8"))


def start_broker(trial_root, kaicc_path, socket_path, log_path):
    if os.path.exists(socket_path):
        os.unlink(socket_path)
    broker_script = os.path.join(_SCRIPTS_DIR, "isolation", "broker.py")
    log_file = open(log_path, "w")
    proc = subprocess.Popen(
        [sys.executable, broker_script, "--trial-root", trial_root, "--kaicc-path", kaicc_path, "--socket-path", socket_path],
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    return proc, log_file


def wait_for_broker_ready(proc, socket_path, log_path, timeout_seconds=10):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise OrchestratorError(f"broker exited before becoming ready (see {log_path})")
        if os.path.exists(socket_path):
            try:
                st = os.stat(socket_path)
                if stat.S_ISSOCK(st.st_mode):
                    with open(log_path) as f:
                        if "READY" in f.read():
                            return
            except OSError:
                pass
        time.sleep(0.05)
    raise OrchestratorError("broker did not become ready in time")


def stop_broker(proc, socket_path, log_file):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    finally:
        try:
            log_file.close()
        except OSError:
            pass
        if os.path.exists(socket_path):
            try:
                os.unlink(socket_path)
            except OSError:
                pass


def stage_toolchain(compiler_root, toolchains_root=None):
    stage_script = os.path.join(_SCRIPTS_DIR, "isolation", "stage-toolchain.sh")
    cmd = [stage_script, "--compiler-root", compiler_root]
    if toolchains_root:
        cmd += ["--toolchains-root", toolchains_root]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise OrchestratorError(f"stage-toolchain.sh failed: {result.stderr}")
    staged_root = result.stdout.strip().splitlines()[-1]
    manifest_path = staged_root + ".manifest.json"
    with open(manifest_path) as f:
        manifest = json.load(f)
    return staged_root, manifest


def run_sandboxed_executable(trial_id, root, timeout_seconds=15):
    """Runs the compiled program inside the M1 sandbox with a REAL,
    verified timeout: sandbox-exec.sh's own `--timeout` flag kills the
    CONTAINER ITSELF by name if it runs too long (see that script's own
    header comment for why merely killing this Python subprocess would
    leave the container running as an orphan - confirmed directly: a
    plain `subprocess.run(..., timeout=...)` against `podman run` does
    NOT stop the container, since podman detaches it from the client
    process via conmon). The outer Python-level timeout below is a
    defense-in-depth fallback ONLY, given generous headroom over the
    script's own internal timeout plus image-build overhead - it should
    essentially never fire in practice."""
    sandbox_exec = os.path.join(_SCRIPTS_DIR, "sandbox-exec.sh")
    cmd = [sandbox_exec, trial_id]
    if root:
        cmd += ["--root", root]
    cmd += ["--timeout", str(timeout_seconds), "--", "/workspace/benchmark_out"]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_seconds + 30)


def run_collector(trial_id, root):
    collect_script = os.path.join(_SCRIPTS_DIR, "collect-isolated-trial.sh")
    cmd = [collect_script, "--trial", trial_id]
    if root:
        cmd += ["--root", root]
    return subprocess.run(cmd, capture_output=True, text=True)


def run_dry_run_validation(task_number, trial_root, kaicc_path, timeout_seconds=20):
    """HOST-ONLY diagnostic. The result is written to a host-only file and
    is NEVER appended to `messages` or otherwise fed back to the adapter -
    the agent loop has already terminated (via `finish` or a limit) by
    the time this ever runs.

    BOUNDED with its own timeout, and kills the ENTIRE process group, not
    just the direct child: the collected source can be ANY KAI program a
    session produced, including one that never terminates (discovered via
    scripts/test-agent-adapter.py's own run-timeout fixture - an earlier
    version of this function hung indefinitely on exactly this case).
    `validate-run.py` itself runs the compiled candidate directly on the
    host with no timeout of its own (a reasonable design for its
    original, human-operator-driven use case - a person validating their
    own candidate solution). A plain `subprocess.run(..., timeout=...)`
    here would only kill validate-run.py's own process on expiry, leaving
    the COMPILED PROGRAM IT SPAWNED (the actual non-terminating
    executable) running as an orphaned grandchild - `start_new_session`
    plus `os.killpg` ensures the whole tree is terminated, so nothing is
    ever left running on the host."""
    collected = os.path.join(trial_root, "host", "result", "benchmark.kai")
    if not os.path.isfile(collected):
        return None
    validate_script = os.path.join(_SCRIPTS_DIR, "validate-run.py")
    proc = subprocess.Popen(
        [sys.executable, validate_script, task_number, collected, "--kaicc", kaicc_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        proc.communicate(timeout=timeout_seconds)
        return {"returncode": proc.returncode, "resultLine": "PASS" if proc.returncode == 0 else "FAIL"}
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        return {
            "returncode": None,
            "resultLine": "TIMEOUT",
            "error": f"dry-run validation exceeded {timeout_seconds}s and was aborted (host-only diagnostic - never fed back to the adapter)",
        }


class ToolContext:
    def __init__(self, workspace_dir, socket_path, trial_id, root, max_source_bytes, run_timeout_seconds=15):
        self.workspace_dir = workspace_dir
        self.socket_path = socket_path
        self.trial_id = trial_id
        self.root = root
        self.max_source_bytes = max_source_bytes
        self.run_timeout_seconds = run_timeout_seconds


def dispatch_tool(tool_name, args, ctx):
    started = time.monotonic()

    def ms():
        return int((time.monotonic() - started) * 1000)

    if tool_name == "read_source":
        try:
            data = read_validated_source(ctx.workspace_dir)
            return {"allowed": True, "content": data.decode("utf-8", errors="replace"), "durationMs": ms(), "resultBytes": len(data)}
        except InvalidSource as exc:
            return {"allowed": False, "error": f"refusing to read benchmark.kai: {exc.reason}", "durationMs": ms(), "resultBytes": 0}

    if tool_name == "replace_source":
        content_bytes = args["content"].encode("utf-8")
        try:
            write_validated_source(ctx.workspace_dir, content_bytes, max_bytes=ctx.max_source_bytes)
            return {"allowed": True, "durationMs": ms(), "resultBytes": len(content_bytes)}
        except InvalidSource as exc:
            return {"allowed": False, "error": f"refusing to replace benchmark.kai: {exc.reason}", "durationMs": ms(), "resultBytes": 0}

    if tool_name in BROKER_ROUTED_OPERATIONS:
        params = {"line": args["line"], "column": args["column"]} if tool_name in POSITION_OPERATIONS else {}
        try:
            response = send_broker_request(ctx.socket_path, tool_name, params)
        except OSError as exc:
            return {"allowed": False, "error": f"could not reach broker: {exc}", "durationMs": ms(), "resultBytes": 0}
        stdout = response.get("stdout", "") or ""
        stderr = response.get("stderr", "") or ""
        return {
            "allowed": bool(response.get("allowed")),
            "error": response.get("error"),
            "exitCode": response.get("exitCode"),
            "stdout": stdout,
            "stderr": stderr,
            "durationMs": ms(),
            "resultBytes": len(stdout.encode("utf-8")) + len(stderr.encode("utf-8")),
        }

    if tool_name == "run":
        if not os.path.isfile(os.path.join(ctx.workspace_dir, "benchmark_out")):
            return {"allowed": False, "error": "benchmark_out does not exist - compile first", "durationMs": ms(), "resultBytes": 0}
        try:
            result = run_sandboxed_executable(ctx.trial_id, ctx.root, timeout_seconds=ctx.run_timeout_seconds)
        except subprocess.TimeoutExpired:
            # Rare fallback path only - see run_sandboxed_executable()'s
            # own docstring for why sandbox-exec.sh's internal --timeout
            # (below) is the primary, verified mechanism.
            return {
                "allowed": True,
                "timedOut": True,
                "exitCode": None,
                "error": f"run exceeded the configured timeout of {ctx.run_timeout_seconds}s",
                "durationMs": ms(),
                "resultBytes": 0,
            }
        # sandbox-exec.sh's own internal --timeout kills the CONTAINER
        # (never merely this client process) with SIGKILL (exit 137) when
        # it runs too long - this is the primary, verified timeout path.
        timed_out = result.returncode == 137
        response = {
            "allowed": True,
            "timedOut": timed_out,
            "exitCode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "durationMs": ms(),
            "resultBytes": len(result.stdout.encode("utf-8")) + len(result.stderr.encode("utf-8")),
        }
        if timed_out:
            response["error"] = f"run exceeded the configured timeout of {ctx.run_timeout_seconds}s"
        return response

    if tool_name == "finish":
        return {"allowed": True, "durationMs": ms(), "resultBytes": 0}

    return {"allowed": False, "error": f"unknown tool: {tool_name!r}", "durationMs": ms(), "resultBytes": 0}


def summarize_result_for_model(tool_name, result):
    """What the model actually sees for a tool result - deliberately does
    NOT include host paths, and never includes hidden validation
    output (there is none available at this point in the loop anyway -
    validation only ever runs after `finish`, host-side)."""
    if not result.get("allowed"):
        return {"error": result.get("error", "denied")}
    if tool_name == "read_source":
        return {"content": result.get("content", "")}
    if tool_name in BROKER_ROUTED_OPERATIONS:
        out = {"exitCode": result.get("exitCode"), "stdout": result.get("stdout", ""), "stderr": result.get("stderr", "")}
        if result.get("error"):
            out["error"] = result["error"]
        return out
    if tool_name == "run":
        return {"exitCode": result.get("exitCode"), "stdout": result.get("stdout", ""), "stderr": result.get("stderr", "")}
    return {"ok": True}


def read_condition(trial_root):
    with open(os.path.join(trial_root, "host", "orchestration.json")) as f:
        data = json.load(f)
    return data["condition"], data["trialId"], data["taskId"]


def read_task_md(workspace_dir):
    with open(os.path.join(workspace_dir, "TASK.md")) as f:
        return f.read()


def run_session(
    trial_root,
    compiler_root,
    adapter,
    limits=None,
    root=None,
    toolchains_root=None,
    run_dry_run_check=True,
    time_source=time.monotonic,
):
    """Runs one deterministic agent-adapter dry-run session against an
    already-prepared trial. Never a formal benchmark trial - see this
    module's own docstring.

    `time_source` (default `time.monotonic`) is the ONLY clock consulted
    for `max_wall_time_seconds` enforcement - injectable purely so
    scripts/test-agent-adapter.py can prove that enforcement
    deterministically, without a real 120-second wait or timing-sensitive
    flakiness. Never used for anything security-relevant beyond this one
    session-wall-clock check; per-tool-call `durationMs` timings are
    unaffected and always use the real clock."""
    limits = {**DEFAULT_LIMITS, **(limits or {})}
    condition, trial_id, task_id = read_condition(trial_root)
    workspace_dir = os.path.join(trial_root, "workspace")
    task_md = read_task_md(workspace_dir)

    agent_dir = os.path.join(trial_root, "host", "agent")
    os.makedirs(agent_dir, exist_ok=True)

    try:
        initial_source_bytes = read_validated_source(workspace_dir)
    except InvalidSource as exc:
        # The trial's own benchmark.kai is already invalid before the
        # session even starts (never happens for a genuinely fresh trial
        # - prepare-isolated-trial.sh always copies a pristine regular
        # file - but this refuses safely rather than crashing regardless
        # of cause). No tool call ever happens; nothing is read or
        # compiled.
        session = {
            "schemaVersion": SCHEMA_VERSION,
            "benchmarkVersion": BENCHMARK_VERSION,
            "trialId": trial_id,
            "condition": condition,
            "dryRun": True,
            "adapterType": adapter.adapter_type,
            "startedAt": now_iso(),
            "finishedAt": now_iso(),
            "terminationReason": "invalid_initial_source",
            "systemPromptSha256": None,
            "userPromptSha256": None,
            "commonToolsSha256": None,
            "compilerVersion": None,
            "compilerSha256": None,
        }
        with open(os.path.join(agent_dir, "session.json"), "w") as f:
            json.dump(session, f, indent=2)
            f.write("\n")
        with open(os.path.join(agent_dir, "transcript.jsonl"), "w") as f:
            f.write(
                json.dumps(
                    {
                        "schemaVersion": SCHEMA_VERSION,
                        "sequence": 1,
                        "timestamp": now_iso(),
                        "event": "session_end",
                        "terminationReason": "invalid_initial_source",
                        "error": f"refusing to read benchmark.kai: {exc.reason}",
                    }
                )
                + "\n"
            )
        return session, None, None

    initial_source = initial_source_bytes.decode("utf-8", errors="replace")

    system_prompt = SYSTEM_PROMPT
    user_prompt = build_user_prompt(task_md, initial_source)
    tools = build_tool_schemas(condition)
    allowed_tool_names = {t["name"] for t in tools}

    system_sha = sha256_of_text(system_prompt)
    user_sha = sha256_of_text(user_prompt)
    common_tools_sha = common_baseline_tools_sha256(condition)

    staged_root, manifest = stage_toolchain(compiler_root, toolchains_root)
    kaicc_path = os.path.join(staged_root, "bin", "kaicc")

    sockets_dir = "/tmp/kai-ai-native-v1/sockets"
    os.makedirs(sockets_dir, exist_ok=True)
    socket_path = os.path.join(sockets_dir, f"{trial_id}.sock")
    log_path = socket_path + ".broker.log"

    transcript_path = os.path.join(agent_dir, "transcript.jsonl")
    session_path = os.path.join(agent_dir, "session.json")
    metrics_path = os.path.join(agent_dir, "metrics.json")

    sequence = [0]

    def log_event(event_type, **fields):
        sequence[0] += 1
        entry = {"schemaVersion": SCHEMA_VERSION, "sequence": sequence[0], "timestamp": now_iso(), "event": event_type, **fields}
        with open(transcript_path, "a") as f:
            f.write(json.dumps(entry) + "\n")

    session = {
        "schemaVersion": SCHEMA_VERSION,
        "benchmarkVersion": BENCHMARK_VERSION,
        "trialId": trial_id,
        "condition": condition,
        "dryRun": True,
        "adapterType": adapter.adapter_type,
        "startedAt": now_iso(),
        "finishedAt": None,
        "terminationReason": None,
        "systemPromptSha256": system_sha,
        "userPromptSha256": user_sha,
        "commonToolsSha256": common_tools_sha,
        "compilerVersion": manifest["compilerVersion"],
        "compilerSha256": manifest["sha256"]["bin/kaicc"],
    }

    metrics = {
        "totalTurns": 0,
        "totalToolCalls": 0,
        "toolCallsByOperation": {},
        "compileCalls": 0,
        "compileFailures": 0,
        "runCalls": 0,
        "semanticQueryCalls": 0,
        "sourceReads": 0,
        "sourceWrites": 0,
        "toolStdoutBytes": 0,
        "toolStderrBytes": 0,
    }

    broker_proc, broker_log = start_broker(trial_root, kaicc_path, socket_path, log_path)
    ctx = ToolContext(workspace_dir, socket_path, trial_id, root, limits["max_source_bytes"], limits["run_timeout_seconds"])
    dry_run_validation = None
    termination_reason = None
    start_time = time_source()

    try:
        wait_for_broker_ready(broker_proc, socket_path, log_path)
        log_event("session_start", trialId=trial_id, condition=condition, dryRun=True, adapterType=adapter.adapter_type)

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ]

        total_tool_calls = 0
        loop_finished = False

        while True:
            if time_source() - start_time > limits["max_wall_time_seconds"]:
                termination_reason = "max_wall_time_exceeded"
                break
            if metrics["totalTurns"] >= limits["max_turns"]:
                termination_reason = "max_turns_exceeded"
                break

            try:
                raw_response = adapter.next_response(messages, tools)
            except Exception as exc:  # noqa: BLE001 - a real provider adapter can raise anything
                termination_reason = "adapter_error"
                log_event("model_response", turn=metrics["totalTurns"] + 1, error=str(exc))
                break

            metrics["totalTurns"] += 1

            try:
                assistant_text, tool_calls, stop_reason, usage = parse_adapter_response(raw_response)
            except ValueError as exc:
                termination_reason = "malformed_response"
                log_event("model_response", turn=metrics["totalTurns"], error=str(exc))
                break

            log_event(
                "model_response",
                turn=metrics["totalTurns"],
                assistantTextBytes=len(assistant_text.encode("utf-8")),
                toolCallCount=len(tool_calls),
                stopReason=stop_reason,
            )
            messages.append({"role": "assistant", "content": assistant_text, "toolCalls": tool_calls})

            if not tool_calls:
                termination_reason = "end_turn_no_tool_calls" if stop_reason == "end_turn" else "no_tool_calls"
                break

            for call in tool_calls:
                total_tool_calls += 1
                if total_tool_calls > limits["max_tool_calls"]:
                    termination_reason = "max_tool_calls_exceeded"
                    loop_finished = True
                    break

                metrics["totalToolCalls"] += 1
                tool_name = call["name"]
                args = call["arguments"]
                correlation_id = str(uuid.uuid4())

                log_event(
                    "tool_call",
                    turn=metrics["totalTurns"],
                    callId=correlation_id,
                    modelCallId=call["id"],
                    tool=tool_name,
                    argsNormalized=normalize_args_for_log(tool_name, args),
                )

                if tool_name not in allowed_tool_names:
                    result = {
                        "allowed": False,
                        "error": f"tool {tool_name!r} is not permitted for this trial's condition ({condition!r})",
                        "durationMs": 0,
                        "resultBytes": 0,
                    }
                else:
                    ok, err = validate_tool_call_args(tool_name, args)
                    if not ok:
                        result = {"allowed": False, "error": err, "durationMs": 0, "resultBytes": 0}
                    else:
                        result = dispatch_tool(tool_name, args, ctx)

                metrics["toolCallsByOperation"][tool_name] = metrics["toolCallsByOperation"].get(tool_name, 0) + 1
                if tool_name == "compile":
                    metrics["compileCalls"] += 1
                    if not result.get("allowed") or result.get("exitCode") != 0:
                        metrics["compileFailures"] += 1
                elif tool_name == "run":
                    metrics["runCalls"] += 1
                elif tool_name in POSITION_OPERATIONS or tool_name in ("inspect", "call-graph"):
                    metrics["semanticQueryCalls"] += 1
                elif tool_name == "read_source":
                    metrics["sourceReads"] += 1
                elif tool_name == "replace_source":
                    metrics["sourceWrites"] += 1
                metrics["toolStdoutBytes"] += len((result.get("stdout") or "").encode("utf-8"))
                metrics["toolStderrBytes"] += len((result.get("stderr") or "").encode("utf-8"))

                log_event(
                    "tool_result",
                    turn=metrics["totalTurns"],
                    callId=correlation_id,
                    tool=tool_name,
                    allowed=bool(result.get("allowed")),
                    durationMs=result.get("durationMs", 0),
                    resultBytes=result.get("resultBytes", 0),
                )

                messages.append(
                    {"role": "tool", "toolCallId": call["id"], "content": summarize_result_for_model(tool_name, result)}
                )

                if tool_name == "finish" and result.get("allowed"):
                    termination_reason = "finish_tool"
                    log_event("finish", turn=metrics["totalTurns"], reason="model_finish")
                    loop_finished = True
                    break

            if loop_finished:
                break

        collect_result = run_collector(trial_id, root)
        if run_dry_run_check and collect_result.returncode == 0:
            dry_run_validation = run_dry_run_validation(
                task_id.replace("task-", ""), trial_root, kaicc_path, timeout_seconds=limits["dry_run_timeout_seconds"]
            )
            if dry_run_validation is not None:
                with open(os.path.join(agent_dir, "dry_run_validation.json"), "w") as f:
                    json.dump(dry_run_validation, f, indent=2)
                    f.write("\n")

    finally:
        stop_broker(broker_proc, socket_path, broker_log)

    session["finishedAt"] = now_iso()
    session["terminationReason"] = termination_reason
    with open(session_path, "w") as f:
        json.dump(session, f, indent=2)
        f.write("\n")

    metrics["wallClockDurationMs"] = int((time_source() - start_time) * 1000)
    metrics["terminationReason"] = termination_reason
    metrics["inputTokens"] = None
    metrics["outputTokens"] = None
    metrics["cachedInputTokens"] = None
    metrics["cost"] = None
    with open(metrics_path, "w") as f:
        json.dump(metrics, f, indent=2)
        f.write("\n")

    log_event(
        "session_end",
        terminationReason=termination_reason,
        totalTurns=metrics["totalTurns"],
        totalToolCalls=metrics["totalToolCalls"],
    )

    return session, metrics, dry_run_validation
