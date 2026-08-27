#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: executes a command inside a prepared
# trial's isolated sandbox container - network disabled, no repository or
# real-HOME bind mount, non-root, read-only root filesystem except
# /workspace and small tmpfs scratch space. See ISOLATION.md for the full
# threat model this enforces (and, explicitly, does not yet enforce).
#
# Usage:
#   sandbox-exec.sh <trial-id> [--root DIR] -- <command> [args...]
#
# <trial-id> must correspond to a directory already prepared by
# prepare-isolated-trial.sh under <root> (default
# $KAI_BENCH_ISOLATED_ROOT or /tmp/kai-ai-native-v1/isolated).
#
# Only <root>/<trial-id>/workspace is ever bind-mounted into the
# container - host/orchestration.json and the repository itself are never
# visible inside the sandbox.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_TAG="kai-bench-sandbox:m1"
CONTAINERFILE="${BENCH_ROOT}/sandbox/Containerfile"

# shellcheck source=isolation/container-engine.sh
source "${SCRIPT_DIR}/isolation/container-engine.sh"
# shellcheck source=isolation/trial-id.sh
source "${SCRIPT_DIR}/isolation/trial-id.sh"

usage() {
    echo "usage: sandbox-exec.sh <trial-id> [--root DIR] -- <command> [args...]" >&2
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

TRIAL_ID="$1"
shift
ROOT="${KAI_BENCH_ISOLATED_ROOT:-/tmp/kai-ai-native-v1/isolated}"

if [[ "${1:-}" == "--root" ]]; then
    ROOT="$2"
    shift 2
fi

if [[ "${1:-}" != "--" ]]; then
    echo "error: expected '--' before the command to run" >&2
    usage
    exit 1
fi
shift

if [[ $# -lt 1 ]]; then
    echo "error: no command given after '--'" >&2
    exit 1
fi

# Validate the trial ID shape before using it in any path/command - never
# interpolate an unvalidated value into a shell command.
validate_trial_id "${TRIAL_ID}"

TRIAL_ROOT="${ROOT}/${TRIAL_ID}"
WORKSPACE_DIR="${TRIAL_ROOT}/workspace"
if [[ ! -d "${WORKSPACE_DIR}" ]]; then
    echo "error: no prepared workspace found at ${WORKSPACE_DIR}" >&2
    exit 1
fi
WORKSPACE_ABS="$(cd "${WORKSPACE_DIR}" && pwd)"

detect_container_engine

echo "==> Building sandbox image (${IMAGE_TAG})" >&2
"${ENGINE}" build -q -t "${IMAGE_TAG}" -f "${CONTAINERFILE}" "${BENCH_ROOT}/sandbox" >/dev/null

# Hard M1 isolation requirements, all enforced on every invocation:
#   --network=none               no network namespace access at all
#   ENGINE_USER_ARGS              non-root, UID matched to the host owner
#                                  of the bind-mounted workspace
#   --cap-drop=ALL                no Linux capabilities beyond the default
#                                  unprivileged set minus everything
#   --security-opt=no-new-privileges
#                                  no setuid/setgid privilege escalation
#   --read-only                   root filesystem is immutable; only
#                                  /workspace (bind mount) and the two
#                                  tmpfs mounts below are writable
#   -v ...:/workspace             ONLY the prepared workspace/ directory -
#                                  never the repository, never real $HOME
#   --env ...                     an explicit allowlist of environment
#                                  variables - no host environment is
#                                  forwarded implicitly
exec "${ENGINE}" run --rm \
    "${ENGINE_USER_ARGS[@]}" \
    --network=none \
    --cap-drop=ALL \
    --security-opt=no-new-privileges \
    --read-only \
    --tmpfs /tmp:rw,size=64m,mode=1777 \
    --tmpfs /home/sandbox:rw,size=16m,mode=1777 \
    --env LANG=C.UTF-8 \
    --env LC_ALL=C.UTF-8 \
    --env PATH=/usr/bin:/bin \
    --env HOME=/home/sandbox \
    -v "${WORKSPACE_ABS}:/workspace:Z" \
    -w /workspace \
    "${IMAGE_TAG}" \
    "$@"
