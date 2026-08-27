#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M2: stages a COPY of a portable KAI-CC
# release tree outside the repository, verifies it, and records host-only
# audit metadata (version, SHA-256 hashes). The host-side tool broker
# (isolation/broker.py) ALWAYS operates on this staged copy - never
# directly on the repository's own dist/ tree - so nothing about how the
# benchmark exercises kaicc depends on the repository checkout remaining
# untouched during a trial.
#
# Usage:
#   stage-toolchain.sh --compiler-root <portable-install-dir> [--toolchains-root DIR]
#
# <portable-install-dir> must have the shape produced by
# scripts/build-release-linux-x86_64.sh (bin/kaicc, lib/kai/
# libkai_runtime.a) - the same required-file check that script's own
# release-artifact verification uses, so this refuses to stage something
# that is not actually a portable release tree (e.g. accidentally
# pointing it at build/ or compiler/ source).
#
# <toolchains-root> defaults to $KAI_BENCH_TOOLCHAINS_ROOT or
# /tmp/kai-ai-native-v1/toolchains - never inside the repository, and
# never mounted into any trial sandbox.
#
# Produces:
#   <toolchains-root>/<staged-id>/            (the staged copy itself)
#   <toolchains-root>/<staged-id>.manifest.json  (host-only audit record:
#       schemaVersion, sourceRoot, stagedRoot, compilerVersion,
#       sha256 of bin/kaicc and each staged lib/kai/* file, stagedAt)
#
# Prints the staged root's absolute path on stdout as the LAST line, so
# callers can capture it with e.g. `STAGED="$(stage-toolchain.sh ... | tail -1)"`.
set -euo pipefail

COMPILER_ROOT=""
TOOLCHAINS_ROOT="${KAI_BENCH_TOOLCHAINS_ROOT:-/tmp/kai-ai-native-v1/toolchains}"

usage() {
    echo "usage: stage-toolchain.sh --compiler-root <portable-install-dir> [--toolchains-root DIR]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiler-root)
            COMPILER_ROOT="$2"
            shift 2
            ;;
        --toolchains-root)
            TOOLCHAINS_ROOT="$2"
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

if [[ -z "${COMPILER_ROOT}" ]]; then
    usage
    exit 1
fi

if [[ ! -d "${COMPILER_ROOT}" ]]; then
    echo "error: --compiler-root does not exist or is not a directory: ${COMPILER_ROOT}" >&2
    exit 1
fi
COMPILER_ROOT_ABS="$(cd "${COMPILER_ROOT}" && pwd)"

# Refuse anything that does not look like an actual portable release tree
# - same shape build-release-linux-x86_64.sh's own required-file check
# already enforces on the artifact it produces.
if [[ ! -f "${COMPILER_ROOT_ABS}/bin/kaicc" ]]; then
    echo "error: ${COMPILER_ROOT_ABS} does not look like a portable KAI-CC release (missing bin/kaicc)" >&2
    exit 1
fi
if [[ ! -f "${COMPILER_ROOT_ABS}/lib/kai/libkai_runtime.a" ]]; then
    echo "error: ${COMPILER_ROOT_ABS} does not look like a portable KAI-CC release (missing lib/kai/libkai_runtime.a)" >&2
    exit 1
fi

mkdir -p "${TOOLCHAINS_ROOT}"

STAGED_ID="toolchain-$(date -u +%Y%m%dT%H%M%SZ)-$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
STAGED_ROOT="${TOOLCHAINS_ROOT}/${STAGED_ID}"
MANIFEST_PATH="${TOOLCHAINS_ROOT}/${STAGED_ID}.manifest.json"

if [[ -e "${STAGED_ROOT}" ]]; then
    echo "error: staged toolchain directory already exists (unexpected collision): ${STAGED_ROOT}" >&2
    exit 1
fi

# COPY - the broker never executes directly out of the repository's own
# dist/ tree, so nothing the benchmark does can be affected by (or
# affect) the repository checkout during a trial.
cp -a "${COMPILER_ROOT_ABS}" "${STAGED_ROOT}"

STAGED_KAICC="${STAGED_ROOT}/bin/kaicc"
if [[ ! -x "${STAGED_KAICC}" ]]; then
    chmod +x "${STAGED_KAICC}"
fi

# Verify the staged copy actually runs, and read the version FROM the
# compiler rather than assuming any particular string.
COMPILER_VERSION="$("${STAGED_KAICC}" --version 2>&1 | awk '{print $2}')"
if [[ -z "${COMPILER_VERSION}" ]]; then
    echo "error: staged kaicc at ${STAGED_KAICC} did not report a version" >&2
    exit 1
fi
echo "==> Staged and verified kaicc ${COMPILER_VERSION}" >&2

KAICC_SHA256="$(sha256sum "${STAGED_KAICC}" | awk '{print $1}')"
RUNTIME_SHA256="$(sha256sum "${STAGED_ROOT}/lib/kai/libkai_runtime.a" | awk '{print $1}')"

# libz3.so.4 is bundled ONLY when the source LLVM build enabled Z3
# support (see the root CMakeLists.txt's own comment) - present for the
# Ubuntu-container-built portable release, absent for an ordinary Fedora
# dev build. Record its hash if present; do not fail if it legitimately
# is not.
Z3_LIB="${STAGED_ROOT}/lib/kai/libz3.so.4"
if [[ -f "${Z3_LIB}" ]]; then
    Z3_SHA256="$(sha256sum "${Z3_LIB}" | awk '{print $1}')"
    Z3_JSON="\"sha256:${Z3_SHA256}\""
    echo "==> Bundled libz3.so.4 present (sha256:${Z3_SHA256})" >&2
else
    Z3_JSON="null"
    echo "==> No bundled libz3.so.4 in this toolchain (this build's LLVM had no Z3 support)" >&2
fi

STAGED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "${MANIFEST_PATH}" <<EOF
{
  "schemaVersion": 1,
  "stagedId": "${STAGED_ID}",
  "sourceRoot": "${COMPILER_ROOT_ABS}",
  "stagedRoot": "${STAGED_ROOT}",
  "stagedAt": "${STAGED_AT}",
  "compilerVersion": "${COMPILER_VERSION}",
  "sha256": {
    "bin/kaicc": "sha256:${KAICC_SHA256}",
    "lib/kai/libkai_runtime.a": "sha256:${RUNTIME_SHA256}",
    "lib/kai/libz3.so.4": ${Z3_JSON}
  }
}
EOF

echo "==> Manifest: ${MANIFEST_PATH}" >&2
echo "${STAGED_ROOT}"
