#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: automated tests proving the sandbox
# substrate's actual runtime behavior - never just reading back the
# wrapper scripts' own text. Prepares real trial workspaces and runs a
# real sandbox container for every check.
#
# Requires: podman or docker, and python3 (already a benchmark dependency
# via validate-run.py/summarize-results.py) for robust JSON assertions.
#
# Usage:
#   test-isolation.sh
#
# Exits 0 only if every check passes. Always cleans up the trials it
# creates, even on failure.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${BENCH_ROOT}/../../.." && pwd)"
PREPARE="${SCRIPT_DIR}/prepare-isolated-trial.sh"
SANDBOX_EXEC="${SCRIPT_DIR}/sandbox-exec.sh"
COLLECT="${SCRIPT_DIR}/collect-isolated-trial.sh"

TEST_ROOT="$(mktemp -d /tmp/kai-ai-native-v1-isolation-test.XXXXXX)"
export KAI_BENCH_ISOLATED_ROOT="${TEST_ROOT}/isolated"

PASS_COUNT=0
FAIL_COUNT=0

check() {
    # check "<description>" <exit-status-to-judge>
    local description="$1"
    local status="$2"
    if [[ "${status}" -eq 0 ]]; then
        echo "PASS: ${description}"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL: ${description}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

cleanup_on_exit() {
    rm -rf "${TEST_ROOT}"
}
trap cleanup_on_exit EXIT

extract_trial_id() {
    # Pulls the trial ID out of prepare-isolated-trial.sh's own stdout
    # ("Prepared isolated trial: <ID>") rather than re-deriving the
    # naming scheme independently.
    sed -n 's/^Prepared isolated trial: //p'
}

echo "=== Section A-F, L: workspace preparation and manifest checks ==="

TRIAL_01_TEXTUAL_OUT="$("${PREPARE}" --task 01 --condition textual 2>&1)"
PREPARE_STATUS=$?
check "prepare-isolated-trial.sh succeeds for task-01/textual" "${PREPARE_STATUS}"
TRIAL_01_TEXTUAL_ID="$(printf '%s\n' "${TRIAL_01_TEXTUAL_OUT}" | extract_trial_id)"
WS_01_TEXTUAL="${KAI_BENCH_ISOLATED_ROOT}/${TRIAL_01_TEXTUAL_ID}/workspace"

TRIAL_01_SEMANTIC_OUT="$("${PREPARE}" --task 01 --condition semantic 2>&1)"
check "prepare-isolated-trial.sh succeeds for task-01/semantic" $?
TRIAL_01_SEMANTIC_ID="$(printf '%s\n' "${TRIAL_01_SEMANTIC_OUT}" | extract_trial_id)"
WS_01_SEMANTIC="${KAI_BENCH_ISOLATED_ROOT}/${TRIAL_01_SEMANTIC_ID}/workspace"

# A. allowed files only - exactly {TASK.md, benchmark.kai, trial.json}
ACTUAL_FILES="$(cd "${WS_01_TEXTUAL}" && find . -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ')"
EXPECTED_FILES="TASK.md benchmark.kai trial.json "
[[ "${ACTUAL_FILES}" == "${EXPECTED_FILES}" ]]
check "workspace contains exactly the allowed files (got: ${ACTUAL_FILES})" $?

# B. no symlinks anywhere in the workspace
SYMLINK_COUNT="$(find "${WS_01_TEXTUAL}" -type l | wc -l)"
[[ "${SYMLINK_COUNT}" -eq 0 ]]
check "workspace contains no symlinks" $?

# C/D/E. reference/, expected/, other task prompts absent
[[ ! -e "${WS_01_TEXTUAL}/reference" ]]
check "workspace does not contain reference/" $?
[[ ! -e "${WS_01_TEXTUAL}/expected" ]]
check "workspace does not contain expected/" $?
[[ ! -e "${WS_01_TEXTUAL}/tasks" && ! -e "${WS_01_TEXTUAL}/task-02.md" && ! -e "${WS_01_TEXTUAL}/task-03.md" ]]
check "workspace does not contain other tasks' prompts" $?

# F. no .git metadata
[[ ! -e "${WS_01_TEXTUAL}/.git" ]]
check "workspace does not contain .git" $?

# L. trial.json manifest contains no hidden answer/reference data - keys
# are exactly the documented small set, values are hashes/labels only.
python3 - "${WS_01_TEXTUAL}/trial.json" <<'PYEOF'
import json, sys
path = sys.argv[1]
with open(path) as f:
    data = json.load(f)
allowed_top_keys = {
    "schemaVersion", "benchmarkVersion", "trialId", "taskId", "condition",
    "createdAt", "inputHashes", "allowedFiles", "toolPolicyId",
}
extra = set(data.keys()) - allowed_top_keys
if extra:
    print(f"unexpected manifest keys: {extra}", file=sys.stderr)
    sys.exit(1)
for key, value in data.get("inputHashes", {}).items():
    if not (isinstance(value, str) and value.startswith("sha256:") and len(value) == len("sha256:") + 64):
        print(f"inputHashes.{key} is not a well-formed sha256 hash: {value!r}", file=sys.stderr)
        sys.exit(1)
sys.exit(0)
PYEOF
check "trial.json manifest schema is exactly the documented small shape (hashes only)" $?

echo
echo "=== Section M: identical starting hashes across conditions, same task ==="

HASHES_MATCH="$(python3 - "${WS_01_TEXTUAL}/trial.json" "${WS_01_SEMANTIC}/trial.json" <<'PYEOF'
import json, sys
a = json.load(open(sys.argv[1]))["inputHashes"]
b = json.load(open(sys.argv[2]))["inputHashes"]
print("match" if a == b else "mismatch")
PYEOF
)"
[[ "${HASHES_MATCH}" == "match" ]]
check "task-01 textual and semantic trials share identical input hashes" $?

echo
echo "=== Section 34: prepare all six task/condition combinations ==="

for task in 01 02 03; do
    for condition in textual semantic; do
        if [[ "${task}" == "01" ]]; then
            continue # already prepared above
        fi
        OUT="$("${PREPARE}" --task "${task}" --condition "${condition}" 2>&1)"
        STATUS=$?
        check "prepare-isolated-trial.sh succeeds for task-${task}/${condition}" "${STATUS}"
        ID="$(printf '%s\n' "${OUT}" | extract_trial_id)"
        WS="${KAI_BENCH_ISOLATED_ROOT}/${ID}/workspace"
        FILES="$(cd "${WS}" && find . -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ')"
        [[ "${FILES}" == "${EXPECTED_FILES}" ]]
        check "task-${task}/${condition} workspace contains exactly the allowed files" $?
    done
    HASHES_MATCH_TASK="$(python3 - "${KAI_BENCH_ISOLATED_ROOT}"/task-${task}-textual-*/workspace/trial.json "${KAI_BENCH_ISOLATED_ROOT}"/task-${task}-semantic-*/workspace/trial.json <<'PYEOF'
import json, sys
a = json.load(open(sys.argv[1]))["inputHashes"]
b = json.load(open(sys.argv[2]))["inputHashes"]
print("match" if a == b else "mismatch")
PYEOF
)"
    [[ "${HASHES_MATCH_TASK}" == "match" ]]
    check "task-${task} textual and semantic trials share identical input hashes" $?
done

echo
echo "=== Sandbox container checks (task-01/textual as the representative trial) ==="
echo "    (deep security checks run once - the sandbox mechanism does not vary by task content;"
echo "     see the final report for exactly what ran against all six trials vs. once.)"

if ! command -v podman >/dev/null 2>&1 && ! command -v docker >/dev/null 2>&1; then
    echo "SKIP: no container engine (podman/docker) found on PATH - sandbox checks cannot run" >&2
    echo
    echo "${PASS_COUNT} passed, ${FAIL_COUNT} failed (sandbox checks skipped)"
    exit 1
fi

REAL_HOME="${HOME}"

# G/21. repository root path is not reachable from inside the sandbox -
# uses the ACTUAL absolute host repo path, not a guessed /repo mountpoint.
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- test ! -e "${REPO_ROOT}" >/dev/null 2>&1
check "repository root path (${REPO_ROOT}) does not exist inside the sandbox" $?

# H/15. real host HOME is not reachable, and $HOME inside is the synthetic one
if [[ "${REAL_HOME}" != "/home/sandbox" ]]; then
    "${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- test ! -e "${REAL_HOME}" >/dev/null 2>&1
    check "real host HOME (${REAL_HOME}) does not exist inside the sandbox" $?
else
    echo "SKIP: host HOME happens to equal the sandbox's synthetic HOME path - test not meaningful here"
fi
SANDBOX_HOME="$("${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- printenv HOME 2>/dev/null)"
[[ "${SANDBOX_HOME}" == "/home/sandbox" ]]
check "\$HOME inside the sandbox is the synthetic /home/sandbox (got: ${SANDBOX_HOME})" $?

# I/12. non-root
SANDBOX_UID="$("${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- id -u 2>/dev/null)"
[[ -n "${SANDBOX_UID}" && "${SANDBOX_UID}" != "0" ]]
check "sandbox process is non-root (uid=${SANDBOX_UID})" $?

# J/20. network is unusable - deterministic raw-IP TCP connect attempt,
# never dependent on a third-party site's availability. Under
# --network=none there is no route to any non-loopback address at all,
# so this fails immediately rather than timing out.
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- timeout 3 bash -c 'cat < /dev/tcp/1.1.1.1/80' >/dev/null 2>&1
NET_STATUS=$?
[[ "${NET_STATUS}" -ne 0 ]]
check "network connection attempt fails inside the sandbox (--network=none)" $?

# K. workspace is writable
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- bash -c 'touch /workspace/.isolation-write-test && rm /workspace/.isolation-write-test' >/dev/null 2>&1
check "workspace is writable inside the sandbox" $?

# F/14/19.N. no git metadata reachable, git commands fail cleanly
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- git rev-parse --is-inside-work-tree >/dev/null 2>&1
GIT_STATUS=$?
[[ "${GIT_STATUS}" -ne 0 ]]
check "'git rev-parse --is-inside-work-tree' fails inside the sandbox (no .git reachable)" $?

"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- git log >/dev/null 2>&1
GIT_LOG_STATUS=$?
[[ "${GIT_LOG_STATUS}" -ne 0 ]]
check "'git log' fails inside the sandbox (no repository history reachable)" $?

# N. direct attempts to read known host material fail
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- cat "${REPO_ROOT}/CMakeLists.txt" >/dev/null 2>&1
CAT_REPO_STATUS=$?
[[ "${CAT_REPO_STATUS}" -ne 0 ]]
check "'cat <repo>/CMakeLists.txt' fails inside the sandbox" $?

"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- ls "${REAL_HOME}" >/dev/null 2>&1
LS_HOME_STATUS=$?
[[ "${LS_HOME_STATUS}" -ne 0 ]]
check "'ls \$REAL_HOME' fails inside the sandbox" $?

echo
echo "=== Result collection: safe collection and exfiltration defenses ==="

# A/B. normal benchmark.kai regular file collects successfully, and its
# collected SHA-256 matches the workspace source.
"${COLLECT}" --trial "${TRIAL_01_TEXTUAL_ID}" >/dev/null 2>&1
check "collect-isolated-trial.sh succeeds for a normal regular-file benchmark.kai" $?

SRC_HASH="$(sha256sum "${WS_01_TEXTUAL}/benchmark.kai" | awk '{print $1}')"
COLLECTED_PATH="${KAI_BENCH_ISOLATED_ROOT}/${TRIAL_01_TEXTUAL_ID}/host/result/benchmark.kai"
COLLECTED_META="${KAI_BENCH_ISOLATED_ROOT}/${TRIAL_01_TEXTUAL_ID}/host/result/collection.json"
[[ -f "${COLLECTED_PATH}" ]]
check "collected benchmark.kai exists at the host-only result path" $?
COLLECTED_HASH="$(sha256sum "${COLLECTED_PATH}" 2>/dev/null | awk '{print $1}')"
[[ -n "${COLLECTED_HASH}" && "${COLLECTED_HASH}" == "${SRC_HASH}" ]]
check "collected benchmark.kai SHA-256 matches the workspace source (${COLLECTED_HASH})" $?

META_HASH="$(python3 -c "import json; print(json.load(open('${COLLECTED_META}'))['sha256'])" 2>/dev/null)"
[[ "${META_HASH}" == "${SRC_HASH}" ]]
check "collection.json records the same SHA-256 as the collected file" $?

# C/D. only the explicitly allowed output is collected - an unrelated
# extra file left in the workspace must never be copied out.
echo "not-part-of-the-benchmark" > "${WS_01_TEXTUAL}/secret-looking-output.txt"
"${COLLECT}" --trial "${TRIAL_01_TEXTUAL_ID}" >/dev/null 2>&1
RESULT_DIR="${KAI_BENCH_ISOLATED_ROOT}/${TRIAL_01_TEXTUAL_ID}/host/result"
RESULT_FILES="$(cd "${RESULT_DIR}" && find . -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ')"
[[ "${RESULT_FILES}" == "benchmark.kai collection.json " ]]
check "only benchmark.kai/collection.json are ever collected (got: ${RESULT_FILES})" $?
[[ ! -e "${RESULT_DIR}/secret-looking-output.txt" ]]
check "an arbitrary extra workspace file is never collected" $?
rm -f "${WS_01_TEXTUAL}/secret-looking-output.txt"

# E. an absolute symlink to a REAL, existing host file must be refused,
# and the host target's content must never appear in the result area.
SECRET_HOST_FILE="$(mktemp /tmp/kai-isolation-test-secret.XXXXXX)"
echo "THIS-MUST-NEVER-BE-COLLECTED" > "${SECRET_HOST_FILE}"
OUT_SYM_ABS="$("${PREPARE}" --task 02 --condition semantic 2>&1)"
TID_SYM_ABS="$(printf '%s\n' "${OUT_SYM_ABS}" | extract_trial_id)"
WS_SYM_ABS="${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_ABS}/workspace"
rm -f "${WS_SYM_ABS}/benchmark.kai"
ln -s "${SECRET_HOST_FILE}" "${WS_SYM_ABS}/benchmark.kai"
"${COLLECT}" --trial "${TID_SYM_ABS}" >/dev/null 2>&1
COLLECT_ABS_SYMLINK_STATUS=$?
[[ "${COLLECT_ABS_SYMLINK_STATUS}" -ne 0 ]]
check "collector refuses an absolute symlink to a real host file" $?
[[ ! -e "${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_ABS}/host/result/benchmark.kai" ]]
check "no file was collected for the absolute-symlink trial" $?
if [[ -f "${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_ABS}/host/result/benchmark.kai" ]]; then
    LEAKED_CONTENT="$(cat "${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_ABS}/host/result/benchmark.kai" 2>/dev/null)"
    [[ "${LEAKED_CONTENT}" != "THIS-MUST-NEVER-BE-COLLECTED" ]]
    check "secret host file content was not leaked into the result area" $?
else
    check "secret host file content was not leaked into the result area (nothing collected at all)" 0
fi
rm -f "${SECRET_HOST_FILE}"

# F. a relative symlink escaping the workspace must also be refused.
OUT_SYM_REL="$("${PREPARE}" --task 03 --condition textual 2>&1)"
TID_SYM_REL="$(printf '%s\n' "${OUT_SYM_REL}" | extract_trial_id)"
WS_SYM_REL="${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_REL}/workspace"
rm -f "${WS_SYM_REL}/benchmark.kai"
ln -s "../../../../etc/passwd" "${WS_SYM_REL}/benchmark.kai"
"${COLLECT}" --trial "${TID_SYM_REL}" >/dev/null 2>&1
COLLECT_REL_SYMLINK_STATUS=$?
[[ "${COLLECT_REL_SYMLINK_STATUS}" -ne 0 ]]
check "collector refuses a relative symlink escaping the workspace" $?
[[ ! -e "${KAI_BENCH_ISOLATED_ROOT}/${TID_SYM_REL}/host/result/benchmark.kai" ]]
check "no file was collected for the relative-symlink-escape trial" $?

# G. a FIFO in place of benchmark.kai must be refused IMMEDIATELY, never
# by hanging (opening a FIFO for reading blocks until a writer connects).
OUT_FIFO="$("${PREPARE}" --task 01 --condition semantic 2>&1)"
TID_FIFO="$(printf '%s\n' "${OUT_FIFO}" | extract_trial_id)"
WS_FIFO="${KAI_BENCH_ISOLATED_ROOT}/${TID_FIFO}/workspace"
rm -f "${WS_FIFO}/benchmark.kai"
mkfifo "${WS_FIFO}/benchmark.kai"
timeout 5 "${COLLECT}" --trial "${TID_FIFO}" >/dev/null 2>&1
COLLECT_FIFO_STATUS=$?
# timeout uses exit code 124 specifically to signal it had to kill the
# process - any OTHER nonzero exit means the collector itself refused
# promptly, which is the required behavior.
[[ "${COLLECT_FIFO_STATUS}" -ne 0 && "${COLLECT_FIFO_STATUS}" -ne 124 ]]
check "collector refuses a FIFO immediately rather than blocking (exit=${COLLECT_FIFO_STATUS})" $?

# H. invalid/path-traversal trial IDs are refused.
"${COLLECT}" --trial "../../etc/passwd" >/dev/null 2>&1
INVALID_TRAVERSAL_STATUS=$?
[[ "${INVALID_TRAVERSAL_STATUS}" -ne 0 ]]
check "collector refuses a path-traversal trial ID ('../../etc/passwd')" $?
"${COLLECT}" --trial "/etc/passwd" >/dev/null 2>&1
INVALID_ABS_STATUS=$?
[[ "${INVALID_ABS_STATUS}" -ne 0 ]]
check "collector refuses an absolute-path trial ID ('/etc/passwd')" $?
"${COLLECT}" --trial "not-a-real-trial-id" >/dev/null 2>&1
INVALID_SHAPE_STATUS=$?
[[ "${INVALID_SHAPE_STATUS}" -ne 0 ]]
check "collector refuses a trial ID with the wrong shape" $?

# I. host/result is never trial-visible on a subsequent sandbox execution
# - only workspace/ is ever bind-mounted, so this should already hold by
# construction; assert it explicitly rather than merely assuming it.
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- test ! -e /workspace/result >/dev/null 2>&1
check "host/result/ is not visible inside the sandbox as /workspace/result" $?
"${SANDBOX_EXEC}" "${TRIAL_01_TEXTUAL_ID}" -- test ! -e /workspace/host >/dev/null 2>&1
check "host/ is not visible inside the sandbox as /workspace/host" $?

echo
echo "=== Summary ==="
echo "${PASS_COUNT} passed, ${FAIL_COUNT} failed"

if [[ "${FAIL_COUNT}" -eq 0 ]]; then
    exit 0
else
    exit 1
fi
