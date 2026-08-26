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
#   dist/kai-linux-x86_64.tar.gz
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
    -v "${REPO_ROOT}:/workspace/kai-cc:Z" \
    -v "${DIST_DIR}:/workspace/dist:Z" \
    -w /workspace/kai-cc \
    "${IMAGE_TAG}" \
    bash -c '
        set -euo pipefail
        cmake -B build-release -S . -G Ninja
        cmake --build build-release
        ctest --test-dir build-release --output-on-failure
        rm -rf "/workspace/dist/'"${ARTIFACT_NAME}"'"
        cmake --install build-release --prefix "/workspace/dist/'"${ARTIFACT_NAME}"'"
    '

echo "==> Portable artifact staged at dist/${ARTIFACT_NAME}/"
find "${DIST_DIR}/${ARTIFACT_NAME}" -type f

if command -v tar >/dev/null 2>&1; then
    echo "==> Producing dist/${ARTIFACT_NAME}.tar.gz"
    tar -C "${DIST_DIR}" -czf "${DIST_DIR}/${ARTIFACT_NAME}.tar.gz" "${ARTIFACT_NAME}"
    echo "    $(du -h "${DIST_DIR}/${ARTIFACT_NAME}.tar.gz" | cut -f1)"
fi

echo "==> Done."
