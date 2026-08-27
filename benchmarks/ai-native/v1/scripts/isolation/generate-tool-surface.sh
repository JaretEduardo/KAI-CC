#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M2: generates the condition-specific
# client-side tool surface that gets bind-mounted READ-ONLY into a trial
# sandbox as /tools.
#
# IMPORTANT: this is convenience/UX only, never the security boundary.
# The generated `_client.py` is a dumb, condition-BLIND JSON-over-Unix-
# socket forwarder present in BOTH conditions - a textual trial could, in
# principle, invoke it directly with any operation name it likes, or
# speak the wire protocol by hand without any of these wrapper scripts at
# all. What actually enforces the textual/semantic split is the HOST
# broker (isolation/broker.py), which independently knows the trial's
# authoritative condition from host/orchestration.json and rejects any
# operation the condition does not permit, regardless of how the request
# arrives. See ISOLATION.md's "Isolation M2" section.
#
# Usage:
#   generate-tool-surface.sh --output DIR --condition <textual|semantic>
#
# The client always connects to the fixed in-sandbox path
# /run/kai-tool-bridge.sock - the orchestrator (tool-sandbox-exec.sh) is
# responsible for bind-mounting the real host socket file to exactly
# that path (a single FILE, at a top-level path with no dependency on
# any other bind-mounted directory, so the container engine never needs
# to auto-create a mount-point subdirectory inside /tools itself - doing
# that was found to leave a container-privileged, host-unremovable
# directory behind under rootless Podman). No host-specific path is ever
# baked into generated sandbox-visible scripts.
#
# Produces, under <DIR>:
#   _client.py       - shared transport client (both conditions, identical)
#   kai-compile       - both conditions, byte-identical
#   kai-inspect       - semantic only
#   kai-definition    - semantic only
#   kai-references    - semantic only
#   kai-callers       - semantic only
#   kai-callees       - semantic only
#   kai-call-graph    - semantic only
set -euo pipefail

OUTPUT_DIR=""
CONDITION=""

usage() {
    echo "usage: generate-tool-surface.sh --output DIR --condition <textual|semantic>" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --condition)
            CONDITION="$2"
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${OUTPUT_DIR}" || -z "${CONDITION}" ]]; then
    usage
    exit 1
fi
if [[ "${CONDITION}" != "textual" && "${CONDITION}" != "semantic" ]]; then
    echo "error: --condition must be 'textual' or 'semantic' (got '${CONDITION}')" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# _client.py: generic transport only - no authorization logic at all.
# Present identically regardless of condition (see this file's own header
# comment for why that is intentional and safe).
cat > "${OUTPUT_DIR}/_client.py" <<'PYEOF'
#!/usr/bin/env python3
"""Generic transport client for the AI-native benchmark's tool broker.

Connects to the fixed Unix-domain socket at /run/kai-tool-bridge.sock,
sends one JSON request, prints the broker's JSON response to stdout, and
exits with the broker-reported exit code (or 1 if the operation was
denied or the broker could not be reached).

This script carries NO authorization logic - it will forward whatever
operation name it is given. That is intentional: the security boundary
is the host-side broker, not this script (see ISOLATION.md). Prefer the
condition-specific kai-* wrapper scripts for normal use.
"""
import json
import socket
import sys

SOCKET_PATH = "/run/kai-tool-bridge.sock"


def main():
    if len(sys.argv) < 2:
        print("usage: _client.py <operation> [--line N] [--column M]", file=sys.stderr)
        return 1
    operation = sys.argv[1]
    params = {}
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--line" and i + 1 < len(args):
            params["line"] = int(args[i + 1])
            i += 2
        elif args[i] == "--column" and i + 1 < len(args):
            params["column"] = int(args[i + 1])
            i += 2
        else:
            i += 1

    request = {"schemaVersion": 1, "operation": operation, "params": params}

    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(SOCKET_PATH)
    except OSError as exc:
        print(f"error: could not connect to tool broker: {exc}", file=sys.stderr)
        return 1

    with sock:
        sock.sendall(json.dumps(request).encode("utf-8"))
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)

    try:
        response = json.loads(b"".join(chunks).decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        print(f"error: malformed broker response: {exc}", file=sys.stderr)
        return 1

    if not response.get("allowed", False):
        print(f"denied: {response.get('error', 'unknown reason')}", file=sys.stderr)
        return 1

    sys.stdout.write(response.get("stdout", ""))
    sys.stderr.write(response.get("stderr", ""))
    exit_code = response.get("exitCode")
    return exit_code if isinstance(exit_code, int) else 1


if __name__ == "__main__":
    sys.exit(main())
PYEOF
chmod 755 "${OUTPUT_DIR}/_client.py"

write_wrapper() {
    local name="$1"
    local operation="$2"
    cat > "${OUTPUT_DIR}/${name}" <<EOF
#!/bin/sh
exec python3 /tools/_client.py "${operation}" "\$@"
EOF
    chmod 755 "${OUTPUT_DIR}/${name}"
}

# Baseline: present in BOTH conditions, byte-for-byte identical - the
# semantic condition must never get a nicer/different compile interface.
write_wrapper "kai-compile" "compile"

if [[ "${CONDITION}" == "semantic" ]]; then
    write_wrapper "kai-inspect" "inspect"
    write_wrapper "kai-definition" "definition"
    write_wrapper "kai-references" "references"
    write_wrapper "kai-callers" "callers"
    write_wrapper "kai-callees" "callees"
    write_wrapper "kai-call-graph" "call-graph"
fi

echo "Generated ${CONDITION} tool surface at ${OUTPUT_DIR}"
