#!/usr/bin/env bash
# Creates a fresh, ISOLATED agent workspace for one benchmark trial, OUTSIDE
# the repository by default.
#
# Usage:
#   prepare-run.sh <task-number> <condition> [run-root]
#
#   <task-number>  01, 02, or 03
#   <condition>    textual or semantic
#   [run-root]     optional explicit output root (overrides
#                  $KAI_BENCH_RUN_ROOT and the default below)
#
# Output root resolution order: [run-root] arg > $KAI_BENCH_RUN_ROOT env var
# > default /tmp/kai-ai-native-v1 (Linux MVP default - see README's
# "Isolation" section for why this deliberately lives outside the repo).
#
# Result:
#   <run-root>/task-<NN>-<condition>[-trial<N>]/
#     benchmark.kai   - pristine copy of baseline/benchmark.kai
#     TASK.md         - the agent-visible task prompt, and NOTHING else
#     result.json     - a prefilled result.json template (schemaVersion 1)
#
# Deliberately NEVER copied into the workspace: reference/ solutions,
# expected/ stdout, README.md, other tasks' prompts, or anything else from
# the repository - an agent given ONLY this directory as its workspace has
# no way to discover the answer or the benchmark's own design notes. If
# the target directory already exists (e.g. a repeat trial of the same
# task/condition), a `-trial2`, `-trial3`, ... suffix is appended so prior
# runs and their result.json are never overwritten.
#
# This script never touches the main repository working tree and never
# requires `git reset` inside it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: prepare-run.sh <task-number: 01|02|03> <condition: textual|semantic> [run-root]" >&2
    exit 1
fi

TASK_NUMBER="$1"
CONDITION="$2"
RUN_ROOT="${3:-${KAI_BENCH_RUN_ROOT:-/tmp/kai-ai-native-v1}}"

if [[ ! "${TASK_NUMBER}" =~ ^0[1-3]$ ]]; then
    echo "error: task-number must be 01, 02, or 03 (got '${TASK_NUMBER}')" >&2
    exit 1
fi

if [[ "${CONDITION}" != "textual" && "${CONDITION}" != "semantic" ]]; then
    echo "error: condition must be 'textual' or 'semantic' (got '${CONDITION}')" >&2
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

mkdir -p "${RUN_ROOT}"

BASE_NAME="task-${TASK_NUMBER}-${CONDITION}"
TARGET="${RUN_ROOT}/${BASE_NAME}"
SUFFIX=2
while [[ -e "${TARGET}" ]]; do
    TARGET="${RUN_ROOT}/${BASE_NAME}-trial${SUFFIX}"
    SUFFIX=$((SUFFIX + 1))
done

mkdir -p "${TARGET}"
cp "${BASELINE}" "${TARGET}/benchmark.kai"
cp "${TASK_PROMPT}" "${TARGET}/TASK.md"

cat > "${TARGET}/result.json" <<EOF
{
  "schemaVersion": 1,
  "benchmark": "kai-ai-native-v1",
  "task": "task-${TASK_NUMBER}",
  "condition": "${CONDITION}",
  "startTime": null,
  "endTime": null,
  "success": null,
  "validationPass": null,
  "metrics": {
    "compilerInvocations": null,
    "failedCompilerInvocations": null,
    "textualSearches": null,
    "semanticQueries": null,
    "sourceReads": null,
    "sourceLinesRead": null,
    "inputTokens": null,
    "outputTokens": null,
    "totalTokens": null
  },
  "notes": ""
}
EOF

echo "Prepared isolated run directory: ${TARGET}"
echo "  benchmark.kai (pristine baseline copy)"
echo "  TASK.md (agent-visible task prompt)"
echo "  result.json (prefilled template to fill in by hand after the trial)"
echo
echo "Open ONLY this directory as the agent's workspace. Do not grant the"
echo "agent access to the KAI-CC repository during a measured trial - it"
echo "contains reference/ solutions and expected/ stdout for this task."
