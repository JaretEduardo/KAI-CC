#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M2: launches a trial's sandbox with the
# condition-specific tool broker running, and tears everything down
# afterward regardless of outcome.
#
# Unlike scripts/sandbox-exec.sh (which runs a raw command against a
# workspace with no compiler access at all), this script additionally:
#   1. stages and verifies a portable kaicc (scripts/isolation/
#      stage-toolchain.sh) - NEVER the repository's own dist/ tree
#      directly, NEVER mounted into the sandbox
#   2. reads the trial's AUTHORITATIVE condition from
#      host/orchestration.json (never workspace/trial.json)
#   3. generates the matching condition-specific /tools client surface
#      (scripts/isolation/generate-tool-surface.sh)
#   4. starts the host-side broker (scripts/isolation/broker.py), which
#      is the actual enforcement boundary
#   5. runs the sandbox command with /workspace, /tools (read-only), and
#      /run/kai-tool-bridge.sock (a single bind-mounted socket FILE, at a
#      top-level path with no dependency on any other mount, read-write)
#      - never the repository, real HOME, staged compiler, or any host/
#      subdirectory. (A nested .../bridge/tool.sock path under the
#      read-only /tools mount was tried first; it forced the container
#      engine to auto-create a mount-point subdirectory INSIDE our own
#      host-generated tools/ directory, left behind owned by a
#      container-privileged UID the host user could not remove under
#      rootless Podman - a top-level socket path avoids this entirely.)
#   6. always stops the broker and removes the socket, even on failure
#
# Usage:
#   tool-sandbox-exec.sh --trial <trial-id> --compiler-root <portable-install> [--root DIR] -- <command> [args...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ISOLATION_DIR="${SCRIPT_DIR}/isolation"
IMAGE_TAG="kai-bench-sandbox:m1"
CONTAINERFILE="${BENCH_ROOT}/sandbox/Containerfile"

# shellcheck source=isolation/container-engine.sh
source "${ISOLATION_DIR}/container-engine.sh"
# shellcheck source=isolation/trial-id.sh
source "${ISOLATION_DIR}/trial-id.sh"

usage() {
    echo "usage: tool-sandbox-exec.sh --trial <trial-id> --compiler-root <portable-install> [--root DIR] -- <command> [args...]" >&2
}

TRIAL_ID=""
COMPILER_ROOT=""
ROOT="${KAI_BENCH_ISOLATED_ROOT:-/tmp/kai-ai-native-v1/isolated}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trial)
            TRIAL_ID="$2"
            shift 2
            ;;
        --compiler-root)
            COMPILER_ROOT="$2"
            shift 2
            ;;
        --root)
            ROOT="$2"
            shift 2
            ;;
        --)
            shift
            break
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

if [[ -z "${TRIAL_ID}" || -z "${COMPILER_ROOT}" || $# -lt 1 ]]; then
    usage
    exit 1
fi

validate_trial_id "${TRIAL_ID}"

TRIAL_ROOT="${ROOT}/${TRIAL_ID}"
WORKSPACE_DIR="${TRIAL_ROOT}/workspace"
ORCHESTRATION_FILE="${TRIAL_ROOT}/host/orchestration.json"
if [[ ! -d "${WORKSPACE_DIR}" ]]; then
    echo "error: no prepared workspace found at ${WORKSPACE_DIR}" >&2
    exit 1
fi
if [[ ! -f "${ORCHESTRATION_FILE}" ]]; then
    echo "error: no host/orchestration.json found for trial ${TRIAL_ID} - was it prepared by prepare-isolated-trial.sh?" >&2
    exit 1
fi
WORKSPACE_ABS="$(cd "${WORKSPACE_DIR}" && pwd)"

CONDITION="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['condition'])" "${ORCHESTRATION_FILE}")"
if [[ "${CONDITION}" != "textual" && "${CONDITION}" != "semantic" ]]; then
    echo "error: host/orchestration.json has no valid condition (got '${CONDITION}')" >&2
    exit 1
fi
echo "==> Trial ${TRIAL_ID}: authoritative condition = ${CONDITION} (from host/orchestration.json)" >&2

# 1. Stage and verify a portable kaicc OUTSIDE the repository - never
# mounted into the sandbox, only used by the host broker below.
STAGED_TOOLCHAIN="$("${ISOLATION_DIR}/stage-toolchain.sh" --compiler-root "${COMPILER_ROOT}" | tail -1)"
KAICC_PATH="${STAGED_TOOLCHAIN}/bin/kaicc"

# 3. Generate the condition-specific /tools client surface into a
# per-trial staging directory (host-visible, but this one IS mounted
# read-only into the sandbox by design - it contains only generated
# client scripts, never the compiler itself).
TOOLS_DIR="${TRIAL_ROOT}/tools"
rm -rf "${TOOLS_DIR}"
"${ISOLATION_DIR}/generate-tool-surface.sh" --output "${TOOLS_DIR}" --condition "${CONDITION}" >/dev/null

# Socket lives at a short, FLAT path outside the (potentially deeply
# nested) trial directory - AF_UNIX socket paths are limited to ~108
# bytes on Linux, and a path like <root>/<trial-id>/bridge/tool.sock
# could exceed that under a long isolated-root. Only this single FILE is
# ever bind-mounted into the sandbox (never its containing host
# directory), so nothing else nearby it becomes sandbox-visible.
SOCKETS_DIR="/tmp/kai-ai-native-v1/sockets"
mkdir -p "${SOCKETS_DIR}"
HOST_SOCKET_PATH="${SOCKETS_DIR}/${TRIAL_ID}.sock"
rm -f "${HOST_SOCKET_PATH}"

detect_container_engine

echo "==> Building sandbox image (${IMAGE_TAG})" >&2
"${ENGINE}" build -q -t "${IMAGE_TAG}" -f "${CONTAINERFILE}" "${BENCH_ROOT}/sandbox" >/dev/null

BROKER_PID=""
BROKER_LOG="$(mktemp /tmp/kai-tool-broker-log.XXXXXX)"

cleanup() {
    local exit_code=$?
    if [[ -n "${BROKER_PID}" ]] && kill -0 "${BROKER_PID}" 2>/dev/null; then
        kill "${BROKER_PID}" 2>/dev/null || true
        wait "${BROKER_PID}" 2>/dev/null || true
    fi
    rm -f "${HOST_SOCKET_PATH}"
    rm -f "${BROKER_LOG}"
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

# 4. Start the host-side broker and wait until its socket is actually
# ready before launching the sandbox - never race the two.
python3 "${ISOLATION_DIR}/broker.py" \
    --trial-root "${TRIAL_ROOT}" \
    --kaicc-path "${KAICC_PATH}" \
    --socket-path "${HOST_SOCKET_PATH}" \
    >"${BROKER_LOG}" 2>&1 &
BROKER_PID=$!

READY=0
for _ in $(seq 1 100); do
    if [[ -S "${HOST_SOCKET_PATH}" ]] && grep -q "^READY$" "${BROKER_LOG}" 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "${BROKER_PID}" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

if [[ "${READY}" -ne 1 ]]; then
    echo "error: tool broker did not become ready in time" >&2
    cat "${BROKER_LOG}" >&2
    exit 1
fi
echo "==> Tool broker ready at ${HOST_SOCKET_PATH}" >&2

# 5. Run the sandbox command. Same hardening as scripts/sandbox-exec.sh
# (network=none, cap-drop=ALL, no-new-privileges, read-only root, non-
# root, explicit env allowlist), PLUS:
#   -v ...tools:/tools:ro                       generated client scripts, read-only
#   -v ...socket:/run/kai-tool-bridge.sock:rw   the single bridge socket file
# Never the repository, real HOME, or the staged compiler.
#
# --security-opt label=disable: investigated directly (see git history/
# ISOLATION.md for the exact debugging trail) - a confined container
# process was denied "Permission denied" connecting to the bridge socket
# EVEN AFTER `:Z` correctly relabeled the socket's on-disk path to
# container_file_t (verified with `ls -Z` before/after). SELinux's
# unix_stream_socket "connectto" check is evaluated against the LISTENING
# PROCESS's own domain (the host broker, running unconfined), not solely
# the path's on-disk label - relabeling the path alone cannot fix this.
# `label=disable` is the standard, narrowly-scoped fix for exactly this
# host<->container Unix-socket pattern: it affects ONLY this container
# invocation's SELinux (MAC) confinement for filesystem/socket access:
# --network=none, --cap-drop=ALL, --security-opt=no-new-privileges,
# --read-only, non-root uid, and every mount restriction below remain
# fully independent and fully enforced (all are DAC/namespace controls,
# not MAC). It is a no-op on non-SELinux hosts (e.g. Docker on Ubuntu
# runners using AppArmor), so this does not need to be engine-conditional.
set +e
"${ENGINE}" run --rm \
    "${ENGINE_USER_ARGS[@]}" \
    --network=none \
    --cap-drop=ALL \
    --security-opt=no-new-privileges \
    --security-opt=label=disable \
    --read-only \
    --tmpfs /tmp:rw,size=64m,mode=1777 \
    --tmpfs /home/sandbox:rw,size=16m,mode=1777 \
    --env LANG=C.UTF-8 \
    --env LC_ALL=C.UTF-8 \
    --env PATH=/usr/bin:/bin \
    --env HOME=/home/sandbox \
    -v "${WORKSPACE_ABS}:/workspace" \
    -v "${TOOLS_DIR}:/tools:ro" \
    -v "${HOST_SOCKET_PATH}:/run/kai-tool-bridge.sock:rw" \
    -w /workspace \
    "${IMAGE_TAG}" \
    "$@"
COMMAND_EXIT=$?
set -e

exit "${COMMAND_EXIT}"
