#!/usr/bin/env python3
"""AI-NATIVE BENCHMARK - ISOLATION M2: host-side condition-aware tool broker.

This process is the ENFORCEMENT BOUNDARY for the textual-vs-semantic tool
split. It runs entirely on the HOST (never inside the sandbox), listens
on a Unix-domain socket, and is the only thing that ever invokes the
staged `kaicc` binary on a trial's behalf.

Authoritative condition: read ONCE at startup from
`<trial-root>/host/orchestration.json` - a file that is NEVER mounted
into the sandbox. The broker never reads `workspace/trial.json` to decide
what a trial may do; that file is sandbox-visible and sandbox-writable,
and is therefore untrusted input, used only for display/audit, never for
authorization.

A trial's condition is fixed for the entire lifetime of one broker
process. There is no operation that lets a client change it.

Protocol (schemaVersion 1), one JSON object per Unix-stream connection:

    request  (client -> broker, then client shuts down its write side):
        {"schemaVersion": 1, "operation": "<name>", "params": {...}}

    response (broker -> client, then broker closes the connection):
        {"schemaVersion": 1, "operation": "<name>", "allowed": bool,
         "exitCode": int|null, "stdout": str, "stderr": str,
         "error": str}   # "error" present only when allowed=false or a
                          # broker-side failure occurred; absent otherwise

Known operations: compile, inspect, definition, references, callers,
callees, call-graph. Every one of them operates ONLY on the trial's own
`workspace/benchmark.kai` - the client can never name an arbitrary path.
`definition`/`references`/`callers`/`callees` take {"line": N, "column": M}
in "params"; the rest take {} (or an absent/empty "params").

Every request, allowed or denied, is appended to a host-only JSON-Lines
transcript at `<trial-root>/host/broker/transcript.jsonl` - never inside
`workspace/`, never sandbox-visible.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

from safe_source import InvalidSource, read_validated_source

SCHEMA_VERSION = 1
MAX_REQUEST_BYTES = 64 * 1024
# Generous bound for draining (and discarding) an oversized request's
# remaining bytes so the connection can be closed gracefully instead of
# reset - see handle_connection(). Never stored or processed; only ever
# counted.
OVERSIZED_DRAIN_CAP_BYTES = 16 * 1024 * 1024
COMPILE_TIMEOUT_SECONDS = 30
QUERY_TIMEOUT_SECONDS = 15

TEXTUAL_OPERATIONS = {"compile"}
SEMANTIC_ONLY_OPERATIONS = {
    "inspect",
    "definition",
    "references",
    "callers",
    "callees",
    "call-graph",
}
ALL_KNOWN_OPERATIONS = TEXTUAL_OPERATIONS | SEMANTIC_ONLY_OPERATIONS
POSITION_OPERATIONS = {"definition", "references", "callers", "callees"}

# Explicit environment allowlist for every kaicc invocation - never the
# broker process's own inherited environment (which may carry the
# operator's shell history, credentials, or other host-specific state).
# PATH must reach the host's real cc/gcc/clang for kaicc's native link
# step; nothing else is forwarded.
SUBPROCESS_ENV = {
    "PATH": "/usr/bin:/bin:/usr/local/bin",
    "LANG": "C.UTF-8",
    "LC_ALL": "C.UTF-8",
}


NO_PARAM_OPERATIONS = {"compile", "inspect", "call-graph"}


class DeniedOperation(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def validate_params(operation, params):
    """Strictly validates params against the operation's exact schema and
    returns the validated params. Unexpected keys, missing required keys,
    and wrong types are ALL rejected - never silently ignored. This is
    the only place client-supplied params are ever trusted: the client
    can never smuggle a source/output path, workspace, trialId,
    condition, executable, command, or environment override through
    params, because no schema below ever accepts such a key at all.

        compile, inspect, call-graph  -> {} (no client parameters)
        definition/references/callers/callees -> exactly
            {"line": <positive int>, "column": <positive int>}
    """
    if operation in NO_PARAM_OPERATIONS:
        if params:
            raise DeniedOperation(
                f"invalid params: operation {operation!r} accepts no parameters (got keys: {sorted(params.keys())})"
            )
        return {}

    if operation in POSITION_OPERATIONS:
        allowed_keys = {"line", "column"}
        actual_keys = set(params.keys())
        extra = actual_keys - allowed_keys
        if extra:
            raise DeniedOperation(f"invalid params: unexpected keys {sorted(extra)}")
        missing = allowed_keys - actual_keys
        if missing:
            raise DeniedOperation(f"invalid params: missing required keys {sorted(missing)}")

        line = params["line"]
        column = params["column"]
        if isinstance(line, bool) or not isinstance(line, int) or line <= 0:
            raise DeniedOperation("invalid params: 'line' must be a positive integer")
        if isinstance(column, bool) or not isinstance(column, int) or column <= 0:
            raise DeniedOperation("invalid params: 'column' must be a positive integer")
        return {"line": line, "column": column}

    # Unreachable in practice - the caller already checks `operation` is
    # in ALL_KNOWN_OPERATIONS before calling this.
    raise DeniedOperation(f"invalid params: unknown operation {operation!r}")


def read_authoritative_condition(trial_root):
    """Reads the condition from host/orchestration.json - the ONLY
    authoritative source. Never reads workspace/trial.json for this."""
    orchestration_path = os.path.join(trial_root, "host", "orchestration.json")
    with open(orchestration_path) as f:
        data = json.load(f)
    condition = data.get("condition")
    if condition not in ("textual", "semantic"):
        raise SystemExit(f"error: host/orchestration.json has no valid condition: {condition!r}")
    return condition, data.get("trialId"), data.get("taskId")


def allowed_operations_for(condition):
    if condition == "textual":
        return set(TEXTUAL_OPERATIONS)
    return set(TEXTUAL_OPERATIONS) | set(SEMANTIC_ONLY_OPERATIONS)


class Broker:
    def __init__(self, trial_root, kaicc_path, socket_path):
        self.trial_root = trial_root
        self.workspace_dir = os.path.join(trial_root, "workspace")
        self.kaicc_path = kaicc_path
        self.socket_path = socket_path
        self.condition, self.trial_id, self.task_id = read_authoritative_condition(trial_root)
        self.allowed_ops = allowed_operations_for(self.condition)
        self.broker_dir = os.path.join(trial_root, "host", "broker")
        self.scratch_dir = os.path.join(self.broker_dir, "scratch")
        self.transcript_path = os.path.join(self.broker_dir, "transcript.jsonl")
        os.makedirs(self.scratch_dir, exist_ok=True)
        self._sequence = 0
        self._sequence_lock = threading.Lock()
        self._transcript_lock = threading.Lock()

    def log(self, message):
        print(f"[broker:{self.trial_id}] {message}", file=sys.stderr, flush=True)

    def next_sequence(self):
        with self._sequence_lock:
            self._sequence += 1
            return self._sequence

    def append_transcript(self, entry):
        with self._transcript_lock:
            with open(self.transcript_path, "a") as f:
                f.write(json.dumps(entry) + "\n")

    def normalized_params(self, operation, params):
        # Only ever emits {} or a clean {"line": int, "column": int} pair
        # - never echoes arbitrary client-supplied keys/values back into
        # the transcript. This matters especially for a REJECTED request
        # (invalid/unexpected params): an attacker's payload can never
        # turn into unbounded or unsafe transcript data, because nothing
        # outside this exact shape is ever returned.
        if operation in POSITION_OPERATIONS and isinstance(params, dict):
            line = params.get("line")
            column = params.get("column")
            line_ok = isinstance(line, int) and not isinstance(line, bool)
            column_ok = isinstance(column, int) and not isinstance(column, bool)
            if line_ok and column_ok:
                return {"line": line, "column": column}
        return {}

    def run_kaicc(self, args, cwd, timeout):
        return subprocess.run(
            [self.kaicc_path, *args],
            cwd=cwd,
            env=dict(SUBPROCESS_ENV),
            capture_output=True,
            text=True,
            timeout=timeout,
            shell=False,
        )

    def handle_compile(self, source_bytes):
        scratch_id = f"req-{self.next_sequence()}-{int(time.time() * 1000)}"
        req_dir = os.path.join(self.scratch_dir, scratch_id)
        os.makedirs(req_dir, exist_ok=True)
        try:
            scratch_source = os.path.join(req_dir, "benchmark.kai")
            with open(scratch_source, "wb") as f:
                f.write(source_bytes)
            scratch_output = os.path.join(req_dir, "benchmark_out")

            result = self.run_kaicc(
                ["benchmark.kai", "-o", "benchmark_out"], cwd=req_dir, timeout=COMPILE_TIMEOUT_SECONDS
            )

            workspace_output = os.path.join(self.workspace_dir, "benchmark_out")
            if result.returncode == 0 and os.path.isfile(scratch_output):
                # Replace any stale prior output only on a successful
                # compile - never leave a half-written file in place.
                tmp_dest = workspace_output + ".tmp"
                shutil.copy(scratch_output, tmp_dest)
                os.chmod(tmp_dest, 0o755)
                os.replace(tmp_dest, workspace_output)
            return result
        finally:
            shutil.rmtree(req_dir, ignore_errors=True)

    def handle_semantic_query(self, operation, source_bytes, params):
        scratch_id = f"req-{self.next_sequence()}-{int(time.time() * 1000)}"
        req_dir = os.path.join(self.scratch_dir, scratch_id)
        os.makedirs(req_dir, exist_ok=True)
        try:
            scratch_source = os.path.join(req_dir, "benchmark.kai")
            with open(scratch_source, "wb") as f:
                f.write(source_bytes)

            if operation in ("inspect", "call-graph"):
                args = [operation, "benchmark.kai", "--json"]
            else:
                # params were already strictly validated by
                # validate_params() before this method was ever called -
                # 'line'/'column' are guaranteed present as positive ints.
                args = [
                    operation,
                    "benchmark.kai",
                    "--line",
                    str(params["line"]),
                    "--column",
                    str(params["column"]),
                    "--json",
                ]

            return self.run_kaicc(args, cwd=req_dir, timeout=QUERY_TIMEOUT_SECONDS)
        finally:
            shutil.rmtree(req_dir, ignore_errors=True)

    def process_request(self, raw_bytes):
        started = time.monotonic()
        sequence = self.next_sequence()
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        operation = None
        params = {}
        allowed = False
        exit_code = None
        stdout_text = ""
        stderr_text = ""
        error_text = None

        try:
            if len(raw_bytes) > MAX_REQUEST_BYTES:
                raise DeniedOperation(f"request exceeds maximum size of {MAX_REQUEST_BYTES} bytes")

            try:
                request = json.loads(raw_bytes.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                raise DeniedOperation(f"malformed request: {exc}")

            if not isinstance(request, dict):
                raise DeniedOperation("malformed request: expected a JSON object")
            if request.get("schemaVersion") != SCHEMA_VERSION:
                raise DeniedOperation(f"unsupported schemaVersion: {request.get('schemaVersion')!r}")

            operation = request.get("operation")
            if not isinstance(operation, str):
                raise DeniedOperation("malformed request: 'operation' must be a string")
            params = request.get("params") or {}
            if not isinstance(params, dict):
                raise DeniedOperation("malformed request: 'params' must be an object")

            if operation not in ALL_KNOWN_OPERATIONS:
                raise DeniedOperation(f"unknown operation: {operation!r}")

            if operation not in self.allowed_ops:
                raise DeniedOperation(
                    f"operation {operation!r} is not permitted for this trial's condition ({self.condition!r})"
                )

            # Strict params schema check - unexpected keys, missing keys,
            # and wrong types are all rejected here, before the source is
            # even read and before any subprocess is ever considered.
            # This is what makes it impossible for a client to smuggle a
            # path/trialId/condition/executable/environment override
            # through params - no schema below ever accepts such a key.
            validated_params = validate_params(operation, params)

            try:
                source_bytes = read_validated_source(self.workspace_dir)
            except InvalidSource as exc:
                raise DeniedOperation(f"refusing to read benchmark.kai: {exc.reason}")

            if operation == "compile":
                result = self.handle_compile(source_bytes)
            else:
                result = self.handle_semantic_query(operation, source_bytes, validated_params)

            allowed = True
            exit_code = result.returncode
            stdout_text = result.stdout
            stderr_text = result.stderr

        except DeniedOperation as exc:
            allowed = False
            error_text = exc.reason
        except subprocess.TimeoutExpired:
            allowed = True
            error_text = "kaicc invocation timed out"
            exit_code = None
        except Exception as exc:  # noqa: BLE001 - broker must never crash on bad client input
            allowed = False
            error_text = f"internal broker error: {exc}"

        duration_ms = int((time.monotonic() - started) * 1000)

        response = {
            "schemaVersion": SCHEMA_VERSION,
            "operation": operation,
            "allowed": allowed,
            "exitCode": exit_code,
            "stdout": stdout_text,
            "stderr": stderr_text,
        }
        if error_text is not None:
            response["error"] = error_text

        self.append_transcript(
            {
                "schemaVersion": SCHEMA_VERSION,
                "sequence": sequence,
                "timestamp": timestamp,
                "trialId": self.trial_id,
                "condition": self.condition,
                "operation": operation,
                "normalizedArgs": self.normalized_params(operation, params) if operation else {},
                "allowed": allowed,
                "exitCode": exit_code,
                "durationMs": duration_ms,
                "stdoutBytes": len(stdout_text.encode("utf-8")),
                "stderrBytes": len(stderr_text.encode("utf-8")),
                "error": error_text,
            }
        )

        return response

    def process_oversized_request(self):
        """Handles a request that exceeded MAX_REQUEST_BYTES while still
        being read off the socket. Never parses, holds onto, or logs the
        oversized payload itself - the connection is rejected before any
        JSON parsing, params validation, or subprocess execution is even
        attempted. The transcript entry is exactly as small and safe as
        any other denied request (see normalized_params)."""
        sequence = self.next_sequence()
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        error_text = f"request exceeds maximum size of {MAX_REQUEST_BYTES} bytes"

        response = {
            "schemaVersion": SCHEMA_VERSION,
            "operation": None,
            "allowed": False,
            "exitCode": None,
            "stdout": "",
            "stderr": "",
            "error": error_text,
        }

        self.append_transcript(
            {
                "schemaVersion": SCHEMA_VERSION,
                "sequence": sequence,
                "timestamp": timestamp,
                "trialId": self.trial_id,
                "condition": self.condition,
                "operation": None,
                "normalizedArgs": {},
                "allowed": False,
                "exitCode": None,
                "durationMs": 0,
                "stdoutBytes": 0,
                "stderrBytes": 0,
                "error": error_text,
            }
        )

        return response

    def handle_connection(self, conn):
        with conn:
            chunks = []
            total = 0
            oversized = False
            conn.settimeout(5.0)
            try:
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_REQUEST_BYTES:
                        # Oversized: stop reading immediately and DROP
                        # everything buffered so far - never hold onto,
                        # parse, or attempt to process an attacker-
                        # supplied oversized payload in any way. No
                        # subprocess is ever considered for this path.
                        oversized = True
                        chunks = []
                        break
                    chunks.append(chunk)
            except socket.timeout:
                pass

            if oversized:
                # Drain and DISCARD whatever the client still has queued,
                # up to a bound - never stored or processed. This is
                # necessary for the client to actually receive our
                # graceful JSON rejection below: closing a socket while
                # unread data remains in its kernel receive queue makes
                # Linux send a TCP RST instead of a clean FIN, which
                # would abort the connection before our response ever
                # arrives (observed directly - see ISOLATION.md). The
                # drain has its own bound and timeout so a client that
                # never stops sending cannot hang this connection
                # indefinitely; a client that keeps streaming past the
                # cap simply gets no response, exactly as if it had been
                # reset - it can never turn into a stored/logged payload
                # either way.
                drained = 0
                conn.settimeout(2.0)
                try:
                    while drained < OVERSIZED_DRAIN_CAP_BYTES:
                        chunk = conn.recv(65536)
                        if not chunk:
                            break
                        drained += len(chunk)
                except socket.timeout:
                    pass
                response = self.process_oversized_request()
            else:
                raw = b"".join(chunks)
                response = self.process_request(raw)
            try:
                conn.sendall(json.dumps(response).encode("utf-8"))
            except OSError:
                pass

    def serve_forever(self):
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(self.socket_path)
        os.chmod(self.socket_path, 0o666)
        server.listen(8)
        self.log(f"listening on {self.socket_path} (condition={self.condition})")
        print("READY", flush=True)
        try:
            while True:
                try:
                    conn, _ = server.accept()
                except OSError:
                    break
                self.handle_connection(conn)
        finally:
            server.close()
            if os.path.exists(self.socket_path):
                os.unlink(self.socket_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trial-root", required=True)
    parser.add_argument("--kaicc-path", required=True)
    parser.add_argument("--socket-path", required=True)
    args = parser.parse_args()

    trial_root = os.path.abspath(args.trial_root)
    kaicc_path = os.path.abspath(args.kaicc_path)
    if not os.path.isfile(kaicc_path):
        raise SystemExit(f"error: kaicc not found at {kaicc_path}")

    broker = Broker(trial_root, kaicc_path, args.socket_path)
    broker.serve_forever()


if __name__ == "__main__":
    main()
