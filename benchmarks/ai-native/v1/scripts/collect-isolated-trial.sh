#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: SAFE RESULT COLLECTION.
#
# Copies ONLY workspace/benchmark.kai out of a prepared trial - after the
# one-shot sandbox command has already exited (see ISOLATION.md's "Result
# collection and the validation boundary" section for why this always
# runs strictly after, never during, sandbox execution - there is no
# concurrent sandbox writer during collection) - into a HOST-ONLY result
# area that is never mounted into any sandbox.
#
# SECURITY: a sandboxed process can freely rewrite the contents of
# /workspace, including replacing benchmark.kai with a symlink to a real
# host file (e.g. an SSH private key) that the sandbox itself could never
# read (no such path is bind-mounted into it), but which a NAIVE host-side
# `cp`/`cat` could accidentally dereference once running unsandboxed on
# the host after the container exits. This script refuses to read/copy
# benchmark.kai unless an lstat() on the HOST (which never follows
# symlinks) confirms it is an ordinary regular file - never a symlink,
# FIFO, socket, device, or directory - and additionally opens it with
# O_NOFOLLOW as a second, TOCTOU-resistant guarantee of the same property.
# A FIFO in particular is refused via the lstat check BEFORE any open() is
# attempted, since opening a FIFO for reading blocks until a writer
# connects - refusal must happen immediately, never by hanging.
#
# Usage:
#   collect-isolated-trial.sh --trial <trial-id> [--root <isolated-root>]
#
# Produces (host-only, never mounted into any sandbox):
#   <root>/<trial-id>/host/result/benchmark.kai
#   <root>/<trial-id>/host/result/collection.json
#
# Collects ONLY benchmark.kai. TASK.md, trial.json, the rest of the
# workspace, the container filesystem, and any other trial-created file
# are never copied - see ISOLATION.md's "Result collection" section.
#
# This script never invokes reference/, expected/, or validate-run.py -
# it only produces a safe host-side copy. Run validate-run.py against the
# collected file (path printed at the end) as a completely separate,
# subsequent host operation.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=isolation/trial-id.sh
source "${SCRIPT_DIR}/isolation/trial-id.sh"

TRIAL_ID=""
ROOT="${KAI_BENCH_ISOLATED_ROOT:-/tmp/kai-ai-native-v1/isolated}"

usage() {
    echo "usage: collect-isolated-trial.sh --trial <trial-id> [--root <isolated-root>]" >&2
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

if [[ -z "${TRIAL_ID}" ]]; then
    usage
    exit 1
fi

# Never trust the trial ID - same strict pattern sandbox-exec.sh and
# cleanup-isolated-trials.sh already use (see isolation/trial-id.sh). The
# pattern permits no '/' character, so this alone rejects "../", a
# leading "/", or any other path-traversal attempt before TRIAL_ID is
# ever used to build a path.
validate_trial_id "${TRIAL_ID}"

# Unlike cleanup-isolated-trials.sh (which performs a destructive `rm -rf`
# and therefore additionally requires ROOT's own path shape to look like
# "kai-ai-native-v1/isolated" as a defense-in-depth guard before deleting
# anything), collection is read+copy only, and TRIAL_ID is already
# strictly regex-validated above (no '/' character permitted at all, so
# no path-traversal component can reach it) - the collector's
# TRIAL_ROOT="${ROOT}/${TRIAL_ID}" is therefore always confined to
# exactly the caller-configured ROOT, whatever it is named. No additional
# ROOT-naming check is needed for a non-destructive operation, and
# requiring one would wrongly reject legitimate custom roots (e.g. a test
# harness's own randomized temp directory) that do not happen to end in
# the literal default "kai-ai-native-v1/isolated" path shape.
TRIAL_ROOT="${ROOT}/${TRIAL_ID}"
WORKSPACE_DIR="${TRIAL_ROOT}/workspace"
if [[ ! -d "${WORKSPACE_DIR}" ]]; then
    echo "error: no prepared workspace found at ${WORKSPACE_DIR}" >&2
    exit 1
fi
WORKSPACE_ABS="$(cd "${WORKSPACE_DIR}" && pwd)"

SRC="${WORKSPACE_ABS}/benchmark.kai"
RESULT_DIR="${TRIAL_ROOT}/host/result"
DEST="${RESULT_DIR}/benchmark.kai"
COLLECTION_META="${RESULT_DIR}/collection.json"

mkdir -p "${RESULT_DIR}"

python3 - "${SRC}" "${WORKSPACE_ABS}" "${DEST}" "${COLLECTION_META}" "${TRIAL_ID}" <<'PYEOF'
import hashlib
import json
import os
import stat
import sys
from datetime import datetime, timezone

src, workspace_dir, dest, collection_meta_path, trial_id = sys.argv[1:6]

try:
    st = os.lstat(src)
except FileNotFoundError:
    print(f"error: {src} does not exist", file=sys.stderr)
    sys.exit(1)
except OSError as exc:
    print(f"error: could not lstat {src}: {exc}", file=sys.stderr)
    sys.exit(1)

if not stat.S_ISREG(st.st_mode):
    if stat.S_ISLNK(st.st_mode):
        kind = "a symlink"
    elif stat.S_ISFIFO(st.st_mode):
        kind = "a FIFO"
    elif stat.S_ISSOCK(st.st_mode):
        kind = "a socket"
    elif stat.S_ISBLK(st.st_mode):
        kind = "a block device"
    elif stat.S_ISCHR(st.st_mode):
        kind = "a character device"
    elif stat.S_ISDIR(st.st_mode):
        kind = "a directory"
    else:
        kind = "not a regular file"
    print(f"error: refusing to collect {src}: it is {kind}, not a regular file", file=sys.stderr)
    sys.exit(1)

# Defense in depth: confirm the file's REAL (symlink-resolved) parent
# directory is still exactly the workspace directory. The check above
# already proves the file itself is not a symlink; this additionally
# guards against a symlinked ANCESTOR directory somewhere above it.
real_workspace = os.path.realpath(workspace_dir)
real_parent = os.path.dirname(os.path.realpath(src))
if real_parent != real_workspace:
    print(f"error: resolved source path escapes the workspace: {src}", file=sys.stderr)
    sys.exit(1)

# O_NOFOLLOW is a second, TOCTOU-resistant guarantee of the same
# regular-file property confirmed above via lstat: if anything replaced
# the path with a symlink between the lstat call and this open, the open
# itself fails outright instead of silently following it.
fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
with os.fdopen(fd, "rb") as f:
    data = f.read()

digest = hashlib.sha256(data).hexdigest()

with open(dest, "wb") as out:
    out.write(data)

meta = {
    "schemaVersion": 1,
    "trialId": trial_id,
    "collectedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "sha256": digest,
}
with open(collection_meta_path, "w") as mf:
    json.dump(meta, mf, indent=2)
    mf.write("\n")

print(f"collected: {dest}")
print(f"sha256: {digest}")
PYEOF

echo "Result collection complete."
echo "  collected file:    ${DEST}"
echo "  collection.json:   ${COLLECTION_META}"
echo
echo "Next (host-side, completely outside any sandbox):"
echo "  python3 ${SCRIPT_DIR}/validate-run.py <task-number> ${DEST}"
