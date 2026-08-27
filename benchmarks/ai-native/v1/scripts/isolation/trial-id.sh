#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: shared trial-ID validation.
#
# Meant to be `source`d by sandbox-exec.sh, cleanup-isolated-trials.sh, and
# collect-isolated-trial.sh - the scripts that accept a caller-supplied
# trial ID and must never trust it - so there is exactly one canonical
# pattern, not several independently-drifting copies. (The pattern
# matches exactly what prepare-isolated-trial.sh generates.)
#
# The pattern permits no '/' character anywhere, so a value matching it
# cannot contain "../", a leading "/", or any other path-traversal
# component - validating against it is sufficient on its own to rule out
# traversal before the value is ever used to build a path.
set -euo pipefail

readonly KAI_BENCH_TRIAL_ID_REGEX='^task-0[1-3]-(textual|semantic)-[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}$'

# validate_trial_id <value> - returns 0 if <value> matches the exact
# trial-ID shape produced by prepare-isolated-trial.sh; prints an error to
# stderr and returns 1 otherwise. Callers should never use an unvalidated
# trial ID in a path or command.
validate_trial_id() {
    local value="$1"
    if [[ ! "${value}" =~ ${KAI_BENCH_TRIAL_ID_REGEX} ]]; then
        echo "error: '${value}' does not look like a trial ID produced by prepare-isolated-trial.sh" >&2
        return 1
    fi
    return 0
}
