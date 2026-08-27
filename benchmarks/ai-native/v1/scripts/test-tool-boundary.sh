#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M2: automated tests proving the
# condition-specific tool boundary is TECHNICALLY enforced, not merely
# named that way. Exercises the real staged kaicc through the real host
# broker over the real Unix-socket bridge - never fakes success by only
# checking that a wrapper script file exists.
#
# Requires: podman or docker, python3, and a portable KAI-CC release tree
# (default: dist/kai-linux-x86_64 relative to the repo root; override with
# --compiler-root).
#
# Usage:
#   test-tool-boundary.sh [--compiler-root DIR]
#
# Exits 0 only if every check passes. Always cleans up the trials/
# toolchains/sockets it creates, even on failure.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${BENCH_ROOT}/../../.." && pwd)"
PREPARE="${SCRIPT_DIR}/prepare-isolated-trial.sh"
TOOL_EXEC="${SCRIPT_DIR}/tool-sandbox-exec.sh"

COMPILER_ROOT="${REPO_ROOT}/dist/kai-linux-x86_64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiler-root)
            COMPILER_ROOT="$2"
            shift 2
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ ! -f "${COMPILER_ROOT}/bin/kaicc" ]]; then
    echo "error: no portable compiler found at ${COMPILER_ROOT} (bin/kaicc missing)." >&2
    echo "       Build one with scripts/build-release-linux-x86_64.sh, or pass --compiler-root." >&2
    exit 1
fi

TEST_ROOT="$(mktemp -d /tmp/kai-ai-native-v1-tool-boundary-test.XXXXXX)"
export KAI_BENCH_ISOLATED_ROOT="${TEST_ROOT}/isolated"
export KAI_BENCH_TOOLCHAINS_ROOT="${TEST_ROOT}/toolchains"

PASS_COUNT=0
FAIL_COUNT=0

check() {
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
    rm -f /tmp/kai-ai-native-v1/sockets/task-*-tool-boundary-test-*.sock 2>/dev/null
}
trap cleanup_on_exit EXIT

extract_trial_id() {
    sed -n 's/^Prepared isolated trial: //p'
}

run_tool() {
    # run_tool <trial-id> -- <command...>   (captures combined stdout+stderr)
    "${TOOL_EXEC}" --trial "$1" --compiler-root "${COMPILER_ROOT}" -- "${@:3}" 2>&1
}

echo "=== Preparing primary textual/semantic probe trials (task-01) ==="
TEXTUAL_OUT="$("${PREPARE}" --task 01 --condition textual 2>&1)"
check "prepare-isolated-trial.sh succeeds for the textual probe trial" $?
TEXTUAL_ID="$(printf '%s\n' "${TEXTUAL_OUT}" | extract_trial_id)"
TEXTUAL_WS="${KAI_BENCH_ISOLATED_ROOT}/${TEXTUAL_ID}/workspace"

SEMANTIC_OUT="$("${PREPARE}" --task 01 --condition semantic 2>&1)"
check "prepare-isolated-trial.sh succeeds for the semantic probe trial" $?
SEMANTIC_ID="$(printf '%s\n' "${SEMANTIC_OUT}" | extract_trial_id)"
SEMANTIC_WS="${KAI_BENCH_ISOLATED_ROOT}/${SEMANTIC_ID}/workspace"

echo
echo "=== Textual tool-surface tests (item 24) ==="

# A. ordinary compilation works
OUT="$(run_tool "${TEXTUAL_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -q "^denied:" && A_OK=1 || A_OK=0
[[ "${A_OK}" -eq 0 && -f "${TEXTUAL_WS}/benchmark_out" ]]
check "textual: ordinary compilation works (kai-compile succeeds, produces benchmark_out)" $?

# B. ordinary compile errors observable - break the copy, try again, then restore
cp "${TEXTUAL_WS}/benchmark.kai" "${TEST_ROOT}/textual-benchmark-backup.kai"
printf 'this is not valid KAI source :::' > "${TEXTUAL_WS}/benchmark.kai"
OUT="$(run_tool "${TEXTUAL_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -qi "error"
check "textual: ordinary compile errors are observable through kai-compile" $?
cp "${TEST_ROOT}/textual-benchmark-backup.kai" "${TEXTUAL_WS}/benchmark.kai"

# C. semantic wrapper commands absent
OUT="$(run_tool "${TEXTUAL_ID}" -- ls /tools)"
echo "${OUT}" | grep -q "kai-inspect"
[[ $? -ne 0 ]]
check "textual: semantic wrapper commands (kai-inspect etc.) are absent from /tools" $?

# D. raw kaicc absent
OUT="$(run_tool "${TEXTUAL_ID}" -- bash -c 'which kaicc 2>&1; find / -xdev -iname kaicc 2>/dev/null')"
[[ -z "$(echo "${OUT}" | grep -v '^==>' | tr -d '[:space:]')" ]]
check "textual: raw kaicc binary is not present/on PATH anywhere in the sandbox" $?

# E/F. compiler installation and source absent
OUT="$(run_tool "${TEXTUAL_ID}" -- bash -c 'find / -xdev \( -iname "libkai_runtime.a" -o -iname "libz3.so*" -o -iname "*.cpp" -o -iname "CMakeLists.txt" \) 2>/dev/null')"
[[ -z "$(echo "${OUT}" | grep -v '^==>' | tr -d '[:space:]')" ]]
check "textual: compiler installation/runtime/source files are absent from the sandbox" $?

# G. direct generic-client semantic operation is denied
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 /tools/_client.py inspect)"
echo "${OUT}" | grep -q "denied:.*not permitted"
check "textual: invoking the generic client directly with 'inspect' is denied" $?

# H. raw hand-crafted socket protocol request for a semantic op is denied
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/kai-tool-bridge.sock')
s.sendall(json.dumps({'schemaVersion':1,'operation':'call-graph','params':{}}).encode())
s.shutdown(socket.SHUT_WR)
data=b''
while True:
    c=s.recv(65536)
    if not c: break
    data+=c
print(data.decode())
")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "not permitted"
check "textual: hand-crafted raw-protocol 'call-graph' request is denied by the host broker" $?

# I. editing trial.json textual -> semantic does not elevate capability
sed -i 's/"textual"/"semantic"/' "${TEXTUAL_WS}/trial.json"
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 /tools/_client.py inspect)"
echo "${OUT}" | grep -q "denied:.*not permitted"
check "textual: tampering workspace/trial.json to 'semantic' does not elevate capability" $?
sed -i 's/"semantic"/"textual"/' "${TEXTUAL_WS}/trial.json"

# J. unknown operation denied
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 /tools/_client.py totally-made-up-operation)"
echo "${OUT}" | grep -q "denied:.*unknown operation"
check "textual: an unknown operation name is denied" $?

# K. malformed request denied
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/kai-tool-bridge.sock')
s.sendall(b'this is not json{{{')
s.shutdown(socket.SHUT_WR)
data=b''
while True:
    c=s.recv(65536)
    if not c: break
    data+=c
print(data.decode())
")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -qi "malformed"
check "textual: a malformed (non-JSON) request is denied" $?

# L. path-injection attempt denied (broker never accepts a client-supplied path)
OUT="$(run_tool "${TEXTUAL_ID}" -- python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/kai-tool-bridge.sock')
s.sendall(json.dumps({'schemaVersion':1,'operation':'compile','params':{'path':'/etc/passwd','source':'../../../etc/passwd'}}).encode())
s.shutdown(socket.SHUT_WR)
data=b''
while True:
    c=s.recv(65536)
    if not c: break
    data+=c
print(data.decode())
")"
if echo "${OUT}" | grep -q '"operation": "compile"' && ! echo "${OUT}" | grep -q "/etc/passwd\|root:"; then
    L_STATUS=0
else
    L_STATUS=1
fi
check "textual: client-supplied path-like params are ignored (broker only ever operates on workspace/benchmark.kai)" "${L_STATUS}"

echo
echo "=== Semantic tool-surface tests (item 25) ==="

# A/B. same ordinary compilation/diagnostics work
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -q "^denied:" && SEM_A=1 || SEM_A=0
[[ "${SEM_A}" -eq 0 && -f "${SEMANTIC_WS}/benchmark_out" ]]
check "semantic: ordinary compilation works (kai-compile succeeds, produces benchmark_out)" $?

cp "${SEMANTIC_WS}/benchmark.kai" "${TEST_ROOT}/semantic-benchmark-backup.kai"
printf 'this is not valid KAI source :::' > "${SEMANTIC_WS}/benchmark.kai"
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -qi "error"
check "semantic: ordinary compile errors are observable through kai-compile" $?
cp "${TEST_ROOT}/semantic-benchmark-backup.kai" "${SEMANTIC_WS}/benchmark.kai"

# C. inspect
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-inspect)"
echo "${OUT}" | grep -q '"symbols"'
check "semantic: kai-inspect returns real symbol data" $?

# D. definition (clamp_i64 is at line 14, column 4 in the baseline)
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-definition --line 14 --column 4)"
echo "${OUT}" | grep -q '"name":"clamp_i64"'
check "semantic: kai-definition resolves a real symbol" $?

# E. references
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-references --line 14 --column 4)"
echo "${OUT}" | grep -q '"references"'
check "semantic: kai-references returns real reference data" $?

# F. callers
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-callers --line 14 --column 4)"
echo "${OUT}" | grep -q '"callers"'
check "semantic: kai-callers returns real caller data" $?

# G. callees
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-callees --line 14 --column 4)"
echo "${OUT}" | grep -q '"callees"'
check "semantic: kai-callees returns real callee data" $?

# H. call-graph
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-call-graph)"
echo "${OUT}" | grep -q '"functions"'
check "semantic: kai-call-graph returns the real call graph" $?

echo
echo "=== Capability equivalence (item 26) ==="

TEXTUAL_HASH="$(sha256sum "${TEXTUAL_WS}/benchmark.kai" | awk '{print $1}')"
SEMANTIC_HASH="$(sha256sum "${SEMANTIC_WS}/benchmark.kai" | awk '{print $1}')"
[[ "${TEXTUAL_HASH}" == "${SEMANTIC_HASH}" ]]
check "textual and semantic probe trials share identical starting benchmark.kai" $?

TEXTUAL_OUT_HASH="$(sha256sum "${TEXTUAL_WS}/benchmark_out" 2>/dev/null | awk '{print $1}')"
SEMANTIC_OUT_HASH="$(sha256sum "${SEMANTIC_WS}/benchmark_out" 2>/dev/null | awk '{print $1}')"
[[ -n "${TEXTUAL_OUT_HASH}" && "${TEXTUAL_OUT_HASH}" == "${SEMANTIC_OUT_HASH}" ]]
check "identical source compiles to byte-identical output in both conditions" $?

echo
echo "=== Host-file exfiltration tests at broker boundary (item 27, 45) ==="

SECRET_FILE="$(mktemp "${TEST_ROOT}/secret.XXXXXX")"
echo "THIS-MUST-NEVER-BE-COMPILED-OR-LEAKED" > "${SECRET_FILE}"

cp "${SEMANTIC_WS}/benchmark.kai" "${TEST_ROOT}/sem-backup-2.kai"
rm -f "${SEMANTIC_WS}/benchmark.kai"
ln -s "${SECRET_FILE}" "${SEMANTIC_WS}/benchmark.kai"
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -q "denied:.*symlink"
check "broker refuses an absolute symlink to a real host file before compiling" $?
! echo "${OUT}" | grep -q "THIS-MUST-NEVER-BE-COMPILED-OR-LEAKED"
check "secret host file content never appears in broker output" $?

rm -f "${SEMANTIC_WS}/benchmark.kai"
ln -s "../../../../etc/passwd" "${SEMANTIC_WS}/benchmark.kai"
OUT="$(run_tool "${SEMANTIC_ID}" -- /tools/kai-compile)"
echo "${OUT}" | grep -q "denied:.*symlink"
check "broker refuses a relative symlink escaping the workspace before compiling" $?

rm -f "${SEMANTIC_WS}/benchmark.kai"
mkfifo "${SEMANTIC_WS}/benchmark.kai"
START_TS=$(date +%s)
OUT="$(timeout 15 "${TOOL_EXEC}" --trial "${SEMANTIC_ID}" --compiler-root "${COMPILER_ROOT}" -- /tools/kai-compile 2>&1)"
FIFO_STATUS=$?
END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))
echo "${OUT}" | grep -q "denied:.*FIFO"
FIFO_DENIED=$?
[[ "${FIFO_DENIED}" -eq 0 && "${FIFO_STATUS}" -ne 124 && "${ELAPSED}" -lt 15 ]]
check "broker refuses a FIFO immediately rather than blocking (elapsed=${ELAPSED}s)" $?

rm -f "${SEMANTIC_WS}/benchmark.kai"
cp "${TEST_ROOT}/sem-backup-2.kai" "${SEMANTIC_WS}/benchmark.kai"
rm -f "${SECRET_FILE}"

echo
echo "=== Toolchain/bridge/transcript invisibility (items 28, 29, 30) ==="

OUT="$(run_tool "${SEMANTIC_ID}" -- bash -c 'find / -xdev -iname "kaicc" -o -iname "libkai_runtime.a" -o -iname "libz3.so*" 2>/dev/null')"
[[ -z "$(echo "${OUT}" | grep -v '^==>' | tr -d '[:space:]')" ]]
check "staged compiler root is not visible anywhere reasonable inside the sandbox" $?

OUT="$(run_tool "${SEMANTIC_ID}" -- ls -la /run/kai-tool-bridge.sock)"
echo "${OUT}" | grep -q "^srw"
check "bridge entry is exactly the socket special file, nothing else" $?

OUT="$(run_tool "${SEMANTIC_ID}" -- bash -c 'test ! -e /workspace/host && echo HOST_HIDDEN')"
echo "${OUT}" | grep -q "HOST_HIDDEN"
check "host/ (transcript, orchestration.json, result/) is not sandbox-visible" $?

OUT="$(run_tool "${SEMANTIC_ID}" -- bash -c 'find / -xdev -iname "transcript.jsonl" -o -iname "orchestration.json" 2>/dev/null')"
[[ -z "$(echo "${OUT}" | grep -v '^==>' | tr -d '[:space:]')" ]]
check "host-side tool transcript is not readable anywhere inside the sandbox" $?

[[ -f "${KAI_BENCH_ISOLATED_ROOT}/${SEMANTIC_ID}/host/broker/transcript.jsonl" ]]
check "host-side tool transcript file exists and was actually written" $?

echo
echo "=== Strict params-schema and oversized-request tests (review round 2) ==="

# Dedicated, never-yet-compiled trials so "no subprocess ran" can be
# checked as a real filesystem side effect (absence of benchmark_out),
# not just by trusting the JSON response's own "allowed" field.
SCHEMA_TEXTUAL_OUT="$("${PREPARE}" --task 02 --condition textual 2>&1)"
check "prepare-isolated-trial.sh succeeds for the params-schema textual trial" $?
SCHEMA_TEXTUAL_ID="$(printf '%s\n' "${SCHEMA_TEXTUAL_OUT}" | extract_trial_id)"
SCHEMA_TEXTUAL_WS="${KAI_BENCH_ISOLATED_ROOT}/${SCHEMA_TEXTUAL_ID}/workspace"

SCHEMA_SEMANTIC_OUT="$("${PREPARE}" --task 02 --condition semantic 2>&1)"
check "prepare-isolated-trial.sh succeeds for the params-schema semantic trial" $?
SCHEMA_SEMANTIC_ID="$(printf '%s\n' "${SCHEMA_SEMANTIC_OUT}" | extract_trial_id)"
SCHEMA_SEMANTIC_WS="${KAI_BENCH_ISOLATED_ROOT}/${SCHEMA_SEMANTIC_ID}/workspace"

raw_request() {
    # raw_request <trial-id> <python-expr-for-request-dict-or-raw-bytes>
    # Speaks the wire protocol directly over a fresh connection - never
    # goes through _client.py or a kai-* wrapper - and prints the
    # broker's raw JSON (or empty string on a hard connection failure).
    local trial="$1"
    local py_body="$2"
    run_tool "${trial}" -- python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/kai-tool-bridge.sock')
payload = ${py_body}
s.sendall(payload)
s.shutdown(socket.SHUT_WR)
data = b''
while True:
    c = s.recv(65536)
    if not c:
        break
    data += c
print(data.decode())
"
}

# A. textual compile request with {"path":"/etc/passwd"} -> rejected
OUT="$(raw_request "${SCHEMA_TEXTUAL_ID}" "b'{\"schemaVersion\":1,\"operation\":\"compile\",\"params\":{\"path\":\"/etc/passwd\"}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "invalid params"
check "A: textual compile with unexpected 'path' param is rejected" $?
[[ ! -f "${SCHEMA_TEXTUAL_WS}/benchmark_out" ]]
check "A: no compiler subprocess ran (benchmark_out was not created)" $?

# B. semantic inspect request with unexpected {"path":...} -> rejected
OUT="$(raw_request "${SCHEMA_SEMANTIC_ID}" "b'{\"schemaVersion\":1,\"operation\":\"inspect\",\"params\":{\"path\":\"/etc/passwd\"}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "invalid params"
check "B: semantic inspect with unexpected 'path' param is rejected" $?

# C. positional semantic operation with missing 'line' -> rejected
OUT="$(raw_request "${SCHEMA_SEMANTIC_ID}" "b'{\"schemaVersion\":1,\"operation\":\"definition\",\"params\":{\"column\":4}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "missing required keys"
check "C: definition with missing 'line' param is rejected" $?

# D. positional semantic operation with wrong type ({"line":"14",...}) -> rejected
OUT="$(raw_request "${SCHEMA_SEMANTIC_ID}" "b'{\"schemaVersion\":1,\"operation\":\"definition\",\"params\":{\"line\":\"14\",\"column\":4}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "must be a positive integer"
check "D: definition with 'line' as a string (wrong type) is rejected" $?

# E. positional semantic operation with an extra key -> rejected
OUT="$(raw_request "${SCHEMA_SEMANTIC_ID}" "b'{\"schemaVersion\":1,\"operation\":\"definition\",\"params\":{\"line\":14,\"column\":4,\"extra\":true}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "unexpected keys"
check "E: definition with an extra unknown key is rejected" $?

# F. valid line/column request still succeeds (clamp_i64 is at line 14, column 4)
OUT="$(raw_request "${SCHEMA_SEMANTIC_ID}" "b'{\"schemaVersion\":1,\"operation\":\"definition\",\"params\":{\"line\":14,\"column\":4}}'")"
echo "${OUT}" | grep -q '"allowed": true' && echo "${OUT}" | grep -q "clamp_i64"
check "F: a valid, well-formed definition request still succeeds" $?

# G. client-supplied {"condition":"semantic"} cannot affect a textual trial
OUT="$(raw_request "${SCHEMA_TEXTUAL_ID}" "b'{\"schemaVersion\":1,\"operation\":\"inspect\",\"params\":{\"condition\":\"semantic\"}}'")"
echo "${OUT}" | grep -q '"allowed": false'
check "G: injecting a 'condition' param cannot make a textual trial run 'inspect'" $?

# H. client-supplied {"trialId":"..."} cannot select another trial
OUT="$(raw_request "${SCHEMA_TEXTUAL_ID}" "b'{\"schemaVersion\":1,\"operation\":\"compile\",\"params\":{\"trialId\":\"${SCHEMA_SEMANTIC_ID}\"}}'")"
echo "${OUT}" | grep -q '"allowed": false' && echo "${OUT}" | grep -q "invalid params"
check "H: injecting a 'trialId' param cannot redirect the broker to another trial" $?
[[ ! -f "${SCHEMA_TEXTUAL_WS}/benchmark_out" ]]
check "H: no compiler subprocess ran for the trialId-injection attempt" $?

echo
echo "=== Oversized-request test (item 1, review round 2) ==="

# Actually communicates with the running broker over the real socket,
# sending a payload well over MAX_REQUEST_BYTES (64 KiB) - never merely
# greps broker.py for the constant.
OVERSIZED_OUT="$(run_tool "${SCHEMA_TEXTUAL_ID}" -- python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/kai-tool-bridge.sock')
s.sendall(b'x' * (200 * 1024))
s.shutdown(socket.SHUT_WR)
data = b''
while True:
    c = s.recv(65536)
    if not c:
        break
    data += c
print(data.decode())
")"
echo "${OVERSIZED_OUT}" | grep -q '"allowed": false' && echo "${OVERSIZED_OUT}" | grep -q "exceeds maximum size"
check "oversized (200 KiB) request is rejected deterministically over the real socket" $?
[[ ! -f "${SCHEMA_TEXTUAL_WS}/benchmark_out" ]]
check "oversized request never launched a compiler subprocess" $?

# Broker remains usable afterward: a fresh connection, on the SAME
# broker process (same trial, same tool-sandbox-exec.sh invocation),
# must still succeed normally.
REUSE_OUT="$(run_tool "${SCHEMA_TEXTUAL_ID}" -- bash -c '
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(\"/run/kai-tool-bridge.sock\")
s.sendall(b\"x\" * (200 * 1024))
s.shutdown(socket.SHUT_WR)
while s.recv(65536): pass
"
/tools/kai-compile
echo COMPILE_EXIT=$?
')"
echo "${REUSE_OUT}" | grep -q "COMPILE_EXIT=0"
check "broker remains usable for a valid request after an oversized one (same broker process)" $?

# Transcript safety: the oversized entry must be small/bounded and must
# never contain the oversized payload itself.
TRANSCRIPT="${KAI_BENCH_ISOLATED_ROOT}/${SCHEMA_TEXTUAL_ID}/host/broker/transcript.jsonl"
OVERSIZED_LINE="$(grep "exceeds maximum size" "${TRANSCRIPT}" | tail -1)"
[[ -n "${OVERSIZED_LINE}" ]]
check "transcript recorded the oversized-request rejection" $?
LINE_LEN="${#OVERSIZED_LINE}"
[[ "${LINE_LEN}" -lt 500 ]]
check "oversized-request transcript entry is small/bounded (${LINE_LEN} bytes, not the 200 KiB payload)" $?
! echo "${OVERSIZED_LINE}" | grep -q "xxxxxxxx"
check "oversized-request transcript entry does not contain the rejected payload" $?

echo
echo "=== All-six task/condition preparation with ordinary compile smoke (item 32) ==="

for task in 01 02 03; do
    for condition in textual semantic; do
        if [[ "${task}" == "01" ]]; then
            continue
        fi
        OUT="$("${PREPARE}" --task "${task}" --condition "${condition}" 2>&1)"
        check "prepare-isolated-trial.sh succeeds for task-${task}/${condition}" $?
        TID="$(printf '%s\n' "${OUT}" | extract_trial_id)"
        TOOL_OUT="$(run_tool "${TID}" -- /tools/kai-compile)"
        WS="${KAI_BENCH_ISOLATED_ROOT}/${TID}/workspace"
        [[ -f "${WS}/benchmark_out" ]]
        check "task-${task}/${condition}: ordinary compile access works" $?
    done
done

echo
echo "=== Summary ==="
echo "${PASS_COUNT} passed, ${FAIL_COUNT} failed"

if [[ "${FAIL_COUNT}" -eq 0 ]]; then
    exit 0
else
    exit 1
fi
