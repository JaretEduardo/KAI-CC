#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: prepares a physically separate,
# container-ready trial workspace OUTSIDE the repository, with a
# machine-readable manifest and content hashes.
#
# This wraps the SAME inputs scripts/prepare-run.sh already uses
# (baseline/benchmark.kai + tasks/task-<NN>.md) rather than redefining
# them, so a textual and a semantic trial for the same task are
# guaranteed to start from byte-identical source - see ISOLATION.md.
#
# This script does NOT itself launch the sandbox container - see
# scripts/sandbox-exec.sh for that. It only prepares the host-side and
# workspace-side directory structure and manifests that a future
# container run will mount.
#
# Usage:
#   prepare-isolated-trial.sh --task <01|02|03> --condition <textual|semantic> [--output-root DIR]
#
# Produces:
#   <output-root>/<trial-id>/
#     host/
#       orchestration.json   - host-only metadata; NEVER mounted into the sandbox
#     workspace/
#       TASK.md               - agent-visible task prompt
#       benchmark.kai         - pristine baseline copy
#       trial.json            - sandbox-visible manifest (schemaVersion 1; hashes only, no answers)
#
# <output-root> defaults to $KAI_BENCH_ISOLATED_ROOT or
# /tmp/kai-ai-native-v1/isolated - a SIBLING of, never inside,
# scripts/prepare-run.sh's own /tmp/kai-ai-native-v1 trial root, so the
# two preparation mechanisms (uncontained vs. containerized) never
# collide.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TASK_NUMBER=""
CONDITION=""
OUTPUT_ROOT="${KAI_BENCH_ISOLATED_ROOT:-/tmp/kai-ai-native-v1/isolated}"

usage() {
    echo "usage: prepare-isolated-trial.sh --task <01|02|03> --condition <textual|semantic> [--output-root DIR]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --task)
            TASK_NUMBER="$2"
            shift 2
            ;;
        --condition)
            CONDITION="$2"
            shift 2
            ;;
        --output-root)
            OUTPUT_ROOT="$2"
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

if [[ -z "${TASK_NUMBER}" || -z "${CONDITION}" ]]; then
    usage
    exit 1
fi

# Validate against KNOWN benchmark metadata before using either value in
# any path or command - never interpolate an unvalidated argument.
if [[ ! "${TASK_NUMBER}" =~ ^0[1-3]$ ]]; then
    echo "error: --task must be 01, 02, or 03 (got '${TASK_NUMBER}')" >&2
    exit 1
fi
if [[ "${CONDITION}" != "textual" && "${CONDITION}" != "semantic" ]]; then
    echo "error: --condition must be 'textual' or 'semantic' (got '${CONDITION}')" >&2
    exit 1
fi

BASELINE="${BENCH_ROOT}/baseline/benchmark.kai"
TASK_PROMPT="${BENCH_ROOT}/tasks/task-${TASK_NUMBER}.md"
if [[ ! -f "${BASELINE}" ]]; then
    echo "error: baseline source not found at ${BASELINE}" >&2
    exit 1
fi
if [[ ! -f "${TASK_PROMPT}" ]]; then
    echo "error: task prompt not found at ${TASK_PROMPT}" >&2
    exit 1
fi

TASK_ID="task-${TASK_NUMBER}"
CREATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
TIMESTAMP_COMPACT="$(date -u +%Y%m%dT%H%M%SZ)"
RANDOM_SUFFIX="$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
TRIAL_ID="${TASK_ID}-${CONDITION}-${TIMESTAMP_COMPACT}-${RANDOM_SUFFIX}"

mkdir -p "${OUTPUT_ROOT}"
TRIAL_ROOT="${OUTPUT_ROOT}/${TRIAL_ID}"
if [[ -e "${TRIAL_ROOT}" ]]; then
    echo "error: trial directory already exists (unexpected collision): ${TRIAL_ROOT}" >&2
    exit 1
fi

HOST_DIR="${TRIAL_ROOT}/host"
WORKSPACE_DIR="${TRIAL_ROOT}/workspace"
mkdir -p "${HOST_DIR}" "${WORKSPACE_DIR}"

# COPY, never symlink - a symlink back into the repository would defeat
# the entire point of a physically separate trial workspace.
cp "${BASELINE}" "${WORKSPACE_DIR}/benchmark.kai"
cp "${TASK_PROMPT}" "${WORKSPACE_DIR}/TASK.md"

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}
BENCHMARK_HASH="$(sha256_of "${WORKSPACE_DIR}/benchmark.kai")"
TASK_HASH="$(sha256_of "${WORKSPACE_DIR}/TASK.md")"

# workspace/trial.json: everything the SANDBOX itself is allowed to see.
# Hashes only - never reference/expected content, never validator logic,
# never scoring data. Keep schemaVersion 1 intentionally small (see
# ISOLATION.md).
cat > "${WORKSPACE_DIR}/trial.json" <<EOF
{
  "schemaVersion": 1,
  "benchmarkVersion": "ai-native-v1",
  "trialId": "${TRIAL_ID}",
  "taskId": "${TASK_ID}",
  "condition": "${CONDITION}",
  "createdAt": "${CREATED_AT}",
  "inputHashes": {
    "benchmark.kai": "sha256:${BENCHMARK_HASH}",
    "TASK.md": "sha256:${TASK_HASH}"
  },
  "allowedFiles": ["TASK.md", "benchmark.kai", "trial.json"],
  "toolPolicyId": "${CONDITION}-v1"
}
EOF

# host/orchestration.json: host-only metadata. NEVER mounted into the
# sandbox - scripts/sandbox-exec.sh only ever mounts workspace/.
cat > "${HOST_DIR}/orchestration.json" <<EOF
{
  "schemaVersion": 1,
  "trialId": "${TRIAL_ID}",
  "taskId": "${TASK_ID}",
  "condition": "${CONDITION}",
  "createdAt": "${CREATED_AT}",
  "hostWorkspacePath": "${WORKSPACE_DIR}",
  "benchmarkRoot": "${BENCH_ROOT}"
}
EOF

echo "Prepared isolated trial: ${TRIAL_ID}"
echo "  workspace: ${WORKSPACE_DIR}"
echo "  host-only metadata: ${HOST_DIR}/orchestration.json (never mounted into the sandbox)"
echo
echo "Next: benchmarks/ai-native/v1/scripts/sandbox-exec.sh ${TRIAL_ID} -- <command>"
echo "See ISOLATION.md before running any real agent trial."
