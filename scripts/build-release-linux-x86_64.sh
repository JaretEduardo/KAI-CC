#!/usr/bin/env bash
# RELEASE HARDENING M1: builds the portable Linux x86_64 KAI-CC release
# artifact inside an Ubuntu 22.04 container (see
# release/linux-x86_64/Containerfile for exactly why that baseline/
# toolchain combination was chosen) - never by reusing the developer's own
# host toolchain, which may be far newer (this project's own Fedora
# development host requires glibc >= 2.38, too new for a general Linux
# x86_64 release).
#
# Usage:
#   scripts/build-release-linux-x86_64.sh
#
# Produces:
#   dist/kai-linux-x86_64/bin/kaicc
#   dist/kai-linux-x86_64/lib/kai/libkai_runtime.a
#   dist/kai-linux-x86_64/lib/kai/libz3.so.4 (only when LLVM was built
#     with Z3 support - see CMakeLists.txt's own comment)
#   dist/kai-linux-x86_64/examples/*.kai (curated, known-good examples
#     only - see examples/README.md; compiled AND run against the
#     just-installed release kaicc below, so a future example that stops
#     matching its documented output fails this script loudly instead of
#     silently shipping broken)
#   dist/kai-linux-x86_64/LICENSE (v0.1.0-alpha.1: the project's own
#     Apache-2.0 license - see the root LICENSE file)
#   dist/kai-linux-x86_64/README.md (RELEASE HARDENING M2.1: so a
#     downloaded tarball is self-describing on its own)
#   dist/kai-linux-x86_64/THIRD_PARTY_NOTICES.md +
#     dist/kai-linux-x86_64/third_party/licenses/{LLVM-LICENSE.txt,
#     Z3-LICENSE.txt,Z3-COPYRIGHT.txt} (RELEASE HARDENING M2.1: the
#     release statically links LLVM and bundles libz3.so.4 - see
#     THIRD_PARTY_NOTICES.md itself - so the factual third-party notices
#     ship alongside the binaries they describe, never left behind in the
#     source repo only). This script FAILS if any of these required files
#     (including LICENSE) is missing from the produced artifact - never a
#     silent partial release.
#   dist/kai-<version>-linux-x86_64.tar.gz (v0.1.0-alpha.1: version is
#     read directly from the just-installed release kaicc's own
#     `--version` output - never a second hard-coded version literal in
#     this script. The staging directory itself stays the unversioned
#     kai-linux-x86_64/, since KAI_RELEASE_ROOT and other tooling
#     reference that fixed name. This script no longer produces the old
#     unversioned dist/kai-linux-x86_64.tar.gz.)
#
# Uses a container-local build directory (build-release/, bind-mounted
# from the repo root) - NEVER the host's own build/ tree - so the
# resulting artifact actually originates from the Ubuntu 22.04
# environment, not whatever the developer last built on their own host.
#
# Podman is preferred (rootless by default, so this script never leaves
# root-owned files in the repository); Docker is an accepted fallback,
# with explicit --user handling so it behaves the same way.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONTAINERFILE="${REPO_ROOT}/release/linux-x86_64/Containerfile"
IMAGE_TAG="kai-release-linux-x86_64"
DIST_DIR="${REPO_ROOT}/dist"
ARTIFACT_NAME="kai-linux-x86_64"

echo "==> Detecting container engine"
if command -v podman >/dev/null 2>&1; then
    ENGINE=podman
    # Rootless Podman preserves the invoking user's ownership on bind
    # mounts by default - no extra --user handling needed.
    RUN_USER_ARGS=()
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
    # Docker (unlike rootless Podman) runs container processes as root by
    # default, which would leave root-owned files under build-release/
    # and dist/ in the repository - run as the invoking host uid/gid
    # instead, exactly as Podman already does for free.
    RUN_USER_ARGS=(--user "$(id -u):$(id -g)")
    echo "    podman not found - using docker (uid:gid ${RUN_USER_ARGS[1]})"
else
    echo "error: neither podman nor docker was found on PATH." >&2
    echo "       Release Hardening M1 requires one of them to build a portable" >&2
    echo "       Linux x86_64 artifact from the Ubuntu 22.04 baseline - this is" >&2
    echo "       a hard requirement of this milestone, not a convenience." >&2
    exit 1
fi
echo "    using: ${ENGINE}"

echo "==> Building release-build image (${IMAGE_TAG})"
"${ENGINE}" build -t "${IMAGE_TAG}" -f "${CONTAINERFILE}" "${REPO_ROOT}"

echo "==> Running clean build + test + install inside the container"
# build-release/ is an isolated, container-produced build directory -
# deliberately never the host's own build/ (see this script's own header
# comment). It is bind-mounted so ctest/build output is inspectable
# afterward if needed, but it is not a release artifact itself (it is
# .gitignore'd, like build/).
rm -rf "${REPO_ROOT}/build-release"
mkdir -p "${DIST_DIR}"

"${ENGINE}" run --rm \
    "${RUN_USER_ARGS[@]}" \
    -e ARTIFACT_NAME="${ARTIFACT_NAME}" \
    -v "${REPO_ROOT}:/workspace/kai-cc:Z" \
    -v "${DIST_DIR}:/workspace/dist:Z" \
    -w /workspace/kai-cc \
    "${IMAGE_TAG}" \
    bash -c '
        set -euo pipefail
        cmake -B build-release -S . -G Ninja
        cmake --build build-release
        ctest --test-dir build-release --output-on-failure

        DEST="/workspace/dist/${ARTIFACT_NAME}"
        rm -rf "${DEST}"
        cmake --install build-release --prefix "${DEST}"

        # RELEASE HARDENING M2: only known-good, end-to-end-verified
        # examples ship in the release tree - see examples/README.md for
        # the full status of every tracked example.kai and why the others
        # are excluded here. Each one is compiled AND RUN against the
        # release kaicc just installed above (never the build tree), with
        # its exact expected stdout asserted, so a future change that
        # silently breaks one of these fails this script instead of
        # shipping a broken example.
        mkdir -p "${DEST}/examples"

        check_release_example() {
            local name="$1"
            local expected="$2"
            cp "examples/${name}" "${DEST}/examples/${name}"
            local actual
            actual="$("${DEST}/bin/kaicc" "examples/${name}" -o /tmp/release_example_check.out && /tmp/release_example_check.out)"
            if [ "${actual}" != "${expected}" ]; then
                echo "error: release example ${name} did not produce the expected stdout" >&2
                printf "  expected: %s\n" "${expected}" >&2
                printf "  actual:   %s\n" "${actual}" >&2
                exit 1
            fi
            echo "  ${name}: OK"
        }

        echo "Verifying curated release examples..."
        check_release_example "hello.kai" "$(printf "Hello from KAI")"
        check_release_example "functions.kai" "$(printf "Hello\nKAI\n42\n84")"
        check_release_example "conditions.kai" "$(printf "adult\npositive\nnegative\nzero")"
        check_release_example "variables.kai" "$(printf "KAI\n0.1\n2026\n1")"
        # KAI LANGUAGE M6 (post-alpha.2): while + a for loop over an
        # integer literal range.
        check_release_example "loops.kai" "$(printf "0\n1\n2\n3\n4\n0\n1\n2\n3\n4")"
        # KAI LANGUAGE M6 (post-alpha.2): recursion + a for-range loop.
        check_release_example "fibonacci.kai" "$(printf "0\n1\n1\n2\n3\n5\n8\n13\n21\n34")"
        # KAI LANGUAGE M7B/M8B/M9 (post-alpha.2): fixed-size arrays -
        # literal creation, checked indexed reads/writes, array function
        # parameters/returns by value, and nested-array indexing.
        check_release_example "arrays.kai" "$(printf "10\n40\n100\n10\n20\n30\n40\n1\n1\n1\n2\n99")"
        # KAI LANGUAGE M10B/M11B (post-alpha.2): immutable slices - a
        # Slice function parameter, len()/checked indexing, and an
        # executable safe Slice RETURN.
        check_release_example "slices.kai" "$(printf "4\n100\n4\n10")"
        rm -f /tmp/release_example_check.out

        # RELEASE HARDENING M2.1 / v0.1.0-alpha.1: root README, project
        # LICENSE, and factual third-party notices ship in every release
        # artifact - see the header comment at the top of this script.
        # Copied verbatim, never renamed or paraphrased.
        cp LICENSE "${DEST}/LICENSE"
        cp README.md "${DEST}/README.md"
        cp THIRD_PARTY_NOTICES.md "${DEST}/THIRD_PARTY_NOTICES.md"
        mkdir -p "${DEST}/third_party/licenses"
        cp third_party/licenses/LLVM-LICENSE.txt "${DEST}/third_party/licenses/LLVM-LICENSE.txt"
        cp third_party/licenses/Z3-LICENSE.txt "${DEST}/third_party/licenses/Z3-LICENSE.txt"
        cp third_party/licenses/Z3-COPYRIGHT.txt "${DEST}/third_party/licenses/Z3-COPYRIGHT.txt"
    '

echo "==> Verifying required release documentation/notice files are present"
# RELEASE HARDENING M2.1 / v0.1.0-alpha.1: never ship a silent partial
# release - fail loudly if the project LICENSE or any factual third-party
# notice/root README didn't make it into the staged artifact.
REQUIRED_FILES=(
    "LICENSE"
    "README.md"
    "THIRD_PARTY_NOTICES.md"
    "third_party/licenses/LLVM-LICENSE.txt"
    "third_party/licenses/Z3-LICENSE.txt"
)
for f in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "${DIST_DIR}/${ARTIFACT_NAME}/${f}" ]; then
        echo "error: required release file missing: ${f}" >&2
        exit 1
    fi
    echo "  ${f}: present"
done

echo "==> Portable artifact staged at dist/${ARTIFACT_NAME}/"
find "${DIST_DIR}/${ARTIFACT_NAME}" -type f

# v0.1.0-alpha.1: derive the archive's version from the just-built,
# just-installed release kaicc itself (single canonical version source -
# see CMakeLists.txt's KAI_CC_VERSION_STRING - never a second literal
# version duplicated here). The staging directory name (kai-linux-x86_64/)
# is left unversioned deliberately: KAI_RELEASE_ROOT (VS Code packaging)
# and other tooling reference it by that fixed name.
KAICC_BIN="${DIST_DIR}/${ARTIFACT_NAME}/bin/kaicc"
KAICC_VERSION="$("${KAICC_BIN}" --version | awk '{print $2}')"
if [ -z "${KAICC_VERSION}" ]; then
    echo "error: could not determine kaicc version from '${KAICC_BIN} --version'" >&2
    exit 1
fi
echo "==> Release kaicc version: ${KAICC_VERSION}"

VERSIONED_ARCHIVE_NAME="kai-${KAICC_VERSION}-linux-x86_64.tar.gz"

if command -v tar >/dev/null 2>&1; then
    echo "==> Producing dist/${VERSIONED_ARCHIVE_NAME}"
    tar -C "${DIST_DIR}" -czf "${DIST_DIR}/${VERSIONED_ARCHIVE_NAME}" "${ARTIFACT_NAME}"
    echo "    $(du -h "${DIST_DIR}/${VERSIONED_ARCHIVE_NAME}" | cut -f1)"

    # Mirrors the Windows release script's own per-archive .sha256
    # sidecar (informational only, same as there - NOT the official
    # release's combined SHA256SUMS. That file covers all four final
    # release assets - this archive, the Windows .zip, and both VSIXes -
    # and is assembled once every asset exists together at actual GitHub
    # Release publish time; see docs/RELEASING.md for that process. This
    # sidecar exists purely so a local/CI run of this script has
    # something to compare against before that final step.)
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "${DIST_DIR}" && sha256sum "${VERSIONED_ARCHIVE_NAME}" | tee "${VERSIONED_ARCHIVE_NAME}.sha256")
    fi
fi

echo "==> Done."
