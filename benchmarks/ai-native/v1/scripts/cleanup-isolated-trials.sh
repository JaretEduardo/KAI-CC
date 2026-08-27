#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: safely removes prepared isolated
# trial workspaces. Refuses to operate on anything that doesn't look like
# a directory this benchmark's own tooling created - never a blind
# `rm -rf` of a caller-supplied path.
#
# Usage:
#   cleanup-isolated-trials.sh --trial <trial-id> [--root DIR]
#   cleanup-isolated-trials.sh --all [--root DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=isolation/trial-id.sh
source "${SCRIPT_DIR}/isolation/trial-id.sh"

ROOT="${KAI_BENCH_ISOLATED_ROOT:-/tmp/kai-ai-native-v1/isolated}"
TRIAL_ID=""
DO_ALL=0

usage() {
    echo "usage: cleanup-isolated-trials.sh --trial <trial-id> [--root DIR]" >&2
    echo "       cleanup-isolated-trials.sh --all [--root DIR]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trial)
            TRIAL_ID="$2"
            shift 2
            ;;
        --root)
            ROOT="$2"
            shift 2
            ;;
        --all)
            DO_ALL=1
            shift
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

if [[ -z "${TRIAL_ID}" && ${DO_ALL} -eq 0 ]]; then
    usage
    exit 1
fi

if [[ ! -d "${ROOT}" ]]; then
    echo "nothing to clean up: ${ROOT} does not exist" >&2
    exit 0
fi

# Refuse to touch anything unless ROOT itself is clearly this benchmark's
# own isolated-trial root - never operate on an arbitrary caller-supplied
# directory that merely happens to exist. This check runs even when a
# custom --root was passed.
BASE_NAME="$(basename "${ROOT}")"
PARENT_NAME="$(basename "$(dirname "${ROOT}")")"
if [[ "${BASE_NAME}" != "isolated" || "${PARENT_NAME}" != "kai-ai-native-v1" ]]; then
    echo "error: refusing to clean up '${ROOT}' - it does not look like a" >&2
    echo "       kai-ai-native-v1/isolated trial root (expected path to end in" >&2
    echo "       '.../kai-ai-native-v1/isolated')." >&2
    exit 1
fi

remove_trial_dir() {
    local dir="$1"
    local name
    name="$(basename "${dir}")"
    if [[ ! "${name}" =~ ${KAI_BENCH_TRIAL_ID_REGEX} ]]; then
        echo "  skipping (does not look like a trial directory): ${dir}" >&2
        return
    fi
    rm -rf -- "${dir}"
    echo "  removed: ${dir}"
}

if [[ ${DO_ALL} -eq 1 ]]; then
    shopt -s nullglob
    for dir in "${ROOT}"/*/; do
        remove_trial_dir "${dir%/}"
    done
    shopt -u nullglob
else
    validate_trial_id "${TRIAL_ID}"
    TARGET="${ROOT}/${TRIAL_ID}"
    if [[ ! -d "${TARGET}" ]]; then
        echo "nothing to clean up: ${TARGET} does not exist" >&2
        exit 0
    fi
    remove_trial_dir "${TARGET}"
fi
