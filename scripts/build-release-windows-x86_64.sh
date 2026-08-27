#!/usr/bin/env bash
# WINDOWS PORTABLE PACKAGE M2: builds, tests, and packages the portable
# Windows x86_64 KAI-CC release artifact.
#
# Unlike scripts/build-release-linux-x86_64.sh, this script does NOT run
# inside a container. The Linux release script uses a container to get an
# OLDER glibc/libstdc++ baseline than the maintainer's own dev host (see
# that script's own header comment) - there is no equivalent "which
# baseline" question on Windows: WINDOWS M1 already established exactly
# one supported Windows toolchain baseline (MSYS2 UCRT64 + LLVM/Clang
# 22.1.8, see .github/workflows/ci.yml's `compiler-windows` job and
# COMPILER_ARCHITECTURE.md), and this script simply assumes it is being
# run FROM INSIDE that same environment (an MSYS2 UCRT64 shell, exactly
# like the `compiler-windows` CI job's `shell: msys2 {0}` steps) - by a
# maintainer with a normal MSYS2 UCRT64 install, or by CI. This is the
# same script both call (WINDOWS PORTABLE PACKAGE M2 spec §26): CI must
# never fork a second, YAML-only build recipe.
#
# Usage (from an MSYS2 UCRT64 shell, e.g. "C:\msys64\ucrt64.exe" or
# msys2/setup-msys2's `shell: msys2 {0}` with `msystem: UCRT64`):
#
#   scripts/build-release-windows-x86_64.sh
#
# Requires (installed via `pacman -S <name>` in the UCRT64 environment -
# see .github/workflows/ci.yml's `compiler-windows` job for the exact CI
# install list, which this script's own environment check partially
# verifies):
#   mingw-w64-ucrt-x86_64-toolchain   (gcc, the host C toolchain driver
#                                      NativeLinker discovers, and
#                                      objdump, used for DLL dependency
#                                      inspection below)
#   mingw-w64-ucrt-x86_64-clang       (clang/clang++, builds kaicc itself)
#   mingw-w64-ucrt-x86_64-llvm        (the LLVM codegen backend)
#   mingw-w64-ucrt-x86_64-cmake
#   mingw-w64-ucrt-x86_64-ninja
#   zip                               (MSYS2 base package, NOT
#                                      mingw-w64-ucrt-x86_64-prefixed -
#                                      `pacman -S zip` - used to produce
#                                      the final .zip in a form any
#                                      Windows user can extract without
#                                      installing 7-Zip themselves)
#
# Produces:
#   build-release-windows/                    (container-free build tree,
#                                              analogous to the Linux
#                                              script's build-release/ -
#                                              never the developer's own
#                                              ordinary build/)
#   dist/kai-windows-x86_64/bin/kaicc.exe
#   dist/kai-windows-x86_64/bin/<bundled non-system DLLs>  (see §5/§6
#     below - the exact set is discovered empirically from kaicc.exe's
#     own PE import table on THIS build, never assumed)
#   dist/kai-windows-x86_64/lib/kai/libkai_runtime.a
#   dist/kai-windows-x86_64/examples/{hello,functions,conditions,variables}.kai
#     (the SAME curated set as the Linux release - see examples/README.md
#     - unless a genuine Windows incompatibility is found; none was as of
#     this milestone)
#   dist/kai-windows-x86_64/{LICENSE,README.md,THIRD_PARTY_NOTICES.md}
#   dist/kai-windows-x86_64/third_party/licenses/...
#   dist/kai-windows-x86_64-dependencies.txt  (build-evidence dependency
#     manifest - kaicc.exe's imports, the full recursive bundled-DLL
#     closure, and which names were classified as Windows-system-provided
#     and excluded - see §12 below; this is proof a human/CI can inspect
#     directly, not just a claim in a report)
#   dist/kai-<version>-windows-x86_64.zip     (archive root is
#     kai-windows-x86_64/, matching the Linux tarball's layout - version
#     is read from the just-installed release kaicc.exe's own `--version`
#     output, never a second hard-coded literal here)
#   dist/kai-<version>-windows-x86_64.zip.sha256  (informational only -
#     this is NOT the official release's SHA256SUMS; that is a future,
#     separate alpha.2 release step)
#
# The staging directory itself is left UNVERSIONED
# (dist/kai-windows-x86_64/), exactly like the Linux release's
# dist/kai-linux-x86_64/, so a future KAI_RELEASE_ROOT-consuming tool
# (e.g. a future Windows VS Code packaging milestone) can reference it by
# a fixed name - see stage-compiler.mjs's own KAI_RELEASE_ROOT handling
# for the existing Linux precedent this mirrors.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${REPO_ROOT}/dist"
ARTIFACT_NAME="kai-windows-x86_64"
BUILD_DIR="${REPO_ROOT}/build-release-windows"
DEST="${DIST_DIR}/${ARTIFACT_NAME}"

echo "==> Verifying MSYS2 UCRT64 environment (WINDOWS PORTABLE PACKAGE M2 spec §3.1)"
# This is the ONE supported Windows toolchain baseline (WINDOWS M1 spec
# §2) - fail loudly and immediately rather than attempting a build under
# MINGW64/MINGW32/MSYS/a plain Windows shell and producing a
# not-actually-portable or not-actually-native artifact.
if [ "${MSYSTEM:-}" != "UCRT64" ]; then
    echo "error: this script must be run from an MSYS2 UCRT64 shell (\$MSYSTEM=UCRT64)." >&2
    echo "       Current \$MSYSTEM: '${MSYSTEM:-<unset>}'" >&2
    echo "       WINDOWS M1/M2's one supported baseline is MSYS2 UCRT64 - see" >&2
    echo "       .github/workflows/ci.yml's compiler-windows job." >&2
    exit 1
fi
if [ -z "${MINGW_PREFIX:-}" ] || [ ! -d "${MINGW_PREFIX}/bin" ]; then
    echo "error: \$MINGW_PREFIX is unset or does not point at a real UCRT64 install." >&2
    echo "       (\$MINGW_PREFIX: '${MINGW_PREFIX:-<unset>}')" >&2
    exit 1
fi
echo "    MSYSTEM=${MSYSTEM}, MINGW_PREFIX=${MINGW_PREFIX}"

for tool in cmake ninja clang clang++ objdump zip sha256sum; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "error: required tool '${tool}' not found on PATH inside this UCRT64 shell." >&2
        echo "       See this script's own header comment for the exact pacman package list." >&2
        exit 1
    fi
done

echo "==> Configuring clean release build (${BUILD_DIR})"
rm -rf "${BUILD_DIR}"
mkdir -p "${DIST_DIR}"
LLVM_CMAKE_DIR="$(llvm-config --cmakedir)"
cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_DIR="${LLVM_CMAKE_DIR}"

echo "==> Building"
cmake --build "${BUILD_DIR}"

echo "==> Running CTest (including native_compilation_tests - the real"
echo "    .kai -> object -> host linker -> .exe -> execute pipeline)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "==> Installing to staging directory (${DEST})"
rm -rf "${DEST}"
cmake --install "${BUILD_DIR}" --prefix "${DEST}"

KAICC_EXE="${DEST}/bin/kaicc.exe"
if [ ! -f "${KAICC_EXE}" ]; then
    echo "error: expected ${KAICC_EXE} after install - CMake's RUNTIME DESTINATION" >&2
    echo "       install rule did not produce it. (kai::cli::resolveNativeExecutablePath()" >&2
    echo "       only affects kaicc's OWN compile-command output naming, not how CMake" >&2
    echo "       itself names/installs the kaicc target - a MinGW/Clang Windows build of" >&2
    echo "       an add_executable(kaicc ...) target is expected to produce kaicc.exe" >&2
    echo "       directly.)" >&2
    exit 1
fi

echo "==> Discovering kaicc.exe's recursive non-system DLL dependency closure"
# WINDOWS PORTABLE PACKAGE M2 spec §5/§6/§7/§8/§9: never guessed - driven
# entirely by objdump's actual PE import table output plus the actual
# on-disk contents of THIS build's own $MINGW_PREFIX/bin. Classification
# rule: a DLL name that exists under $MINGW_PREFIX/bin is a UCRT64/MSYS2-
# built runtime dependency that must be bundled (this build environment
# never introduces a third category - a native UCRT64 Clang-built
# executable's non-system imports come from $MINGW_PREFIX/bin or nowhere
# else); a name that does NOT exist there is treated as Windows-system-
# provided and left for Windows itself to resolve. Recurses into every
# bundled DLL's own imports until no new names appear (closure).
MANIFEST="${DIST_DIR}/${ARTIFACT_NAME}-dependencies.txt"
{
    echo "KAI-CC Windows x86_64 portable release - DLL dependency manifest"
    echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "MINGW_PREFIX: ${MINGW_PREFIX}"
    echo
} > "${MANIFEST}"

declare -A VISITED=()
declare -A BUNDLED=()
declare -A SYSTEM_EXCLUDED=()
QUEUE=("${KAICC_EXE}")

classify_bundled_name() {
    # Best-effort, human-facing categorization only (WINDOWS PORTABLE
    # PACKAGE M2 spec §5's A-G scheme) - never affects bundle/no-bundle
    # decisions above, which are purely the on-disk-existence check.
    local lower
    lower="$(echo "$1" | tr '[:upper:]' '[:lower:]')"
    case "${lower}" in
        libllvm*|llvm-*) echo "C. LLVM dependency" ;;
        libz3*|z3*)      echo "D. Z3 dependency" ;;
        zlib1*|libzstd*|libzstd1*) echo "E. zlib/zstd dependency" ;;
        libstdc++*|libgcc*|libwinpthread*|libc++*) echo "F. compiler-runtime dependency" ;;
        msys-2.0.dll)    echo "STOP. MSYS runtime - see below" ;;
        *)               echo "G. unknown - requires investigation" ;;
    esac
}

while [ "${#QUEUE[@]}" -gt 0 ]; do
    current="${QUEUE[0]}"
    QUEUE=("${QUEUE[@]:1}")

    imports="$(objdump -p "${current}" | grep -i 'DLL Name:' | awk '{print $NF}')"
    while IFS= read -r dll; do
        [ -z "${dll}" ] && continue
        key="$(echo "${dll}" | tr '[:upper:]' '[:lower:]')"
        if [ -n "${VISITED[${key}]+x}" ]; then
            continue
        fi
        VISITED[${key}]=1

        if [ "${key}" = "msys-2.0.dll" ]; then
            echo "error: kaicc.exe (or a dependency) requires msys-2.0.dll." >&2
            echo "       WINDOWS PORTABLE PACKAGE M2 spec §9: STOP - a bundled" >&2
            echo "       msys-2.0.dll would mean this is not actually a native" >&2
            echo "       Windows/UCRT64 executable. Investigate the build (a stray" >&2
            echo "       MSYS-mode compile instead of UCRT64) before packaging." >&2
            exit 1
        fi

        candidate="${MINGW_PREFIX}/bin/${dll}"
        if [ -f "${candidate}" ]; then
            classification="$(classify_bundled_name "${dll}")"
            BUNDLED[${dll}]="${classification}"
            cp -f "${candidate}" "${DEST}/bin/${dll}"
            QUEUE+=("${candidate}")
            echo "  bundled:  ${dll}  [${classification}]"
        else
            SYSTEM_EXCLUDED[${dll}]=1
        fi
    done <<< "${imports}"
done

{
    echo "kaicc.exe direct + recursive non-system DLL closure (bundled into bin/):"
    if [ "${#BUNDLED[@]}" -eq 0 ]; then
        echo "  (none - kaicc.exe has no non-system DLL dependencies)"
    else
        for name in "${!BUNDLED[@]}"; do
            echo "  ${name}  ->  ${BUNDLED[${name}]}"
        done
    fi
    echo
    echo "Windows/system-provided DLLs referenced (excluded, not bundled):"
    for name in "${!SYSTEM_EXCLUDED[@]}"; do
        echo "  ${name}"
    done
} >> "${MANIFEST}"

echo "==> Dependency manifest written to ${MANIFEST}"
cat "${MANIFEST}"

echo "==> Verifying legal/license material for every bundled DLL (WINDOWS"
echo "    PORTABLE PACKAGE M2.1 spec §12/§13 - fail closed, never ship an"
echo "    unaudited dependency silently)"
# Explicit, audited mapping from a bundled DLL's exact filename to the
# third_party/licenses/ file(s) it requires - kept intentionally small and
# hand-maintained (spec §12: "do not build a giant generic license-
# scanning framework") rather than derived automatically from anything.
# If the MSYS2 toolchain packages change and introduce a DLL not listed
# here, this is exactly the "unknown bundled DLL" case spec §13 requires
# to fail the build rather than ship it unreviewed.
declare -A DLL_LICENSE_FILES=(
    ["libgcc_s_seh-1.dll"]="GCC-COPYING3.txt GCC-RUNTIME-LIBRARY-EXCEPTION.txt"
    ["libstdc++-6.dll"]="GCC-COPYING3.txt GCC-RUNTIME-LIBRARY-EXCEPTION.txt"
    ["libwinpthread-1.dll"]="WINPTHREADS-COPYING.txt"
    ["zlib1.dll"]="ZLIB-LICENSE.txt"
    ["libzstd.dll"]="ZSTD-LICENSE.txt ZSTD-COPYING.txt"
)

# LLVM-LICENSE.txt ships unconditionally (not DLL-keyed): kaicc.exe
# statically incorporates LLVM object code on Windows exactly like the
# Linux release (WINDOWS PORTABLE PACKAGE M2 confirmed no libLLVM*.dll in
# the closure - see THIRD_PARTY_NOTICES.md's LLVM section), so the notice
# applies regardless of which DLLs happen to be bundled. Z3-LICENSE.txt/
# Z3-COPYRIGHT.txt are deliberately NOT in this list - the Windows build
# does not link Z3 at all (no libz3*.dll in the closure), so shipping
# Z3's license here would misrepresent what this artifact contains.
LEGAL_FILES_NEEDED=("LLVM-LICENSE.txt")

for name in "${!BUNDLED[@]}"; do
    key="$(echo "${name}" | tr '[:upper:]' '[:lower:]')"
    mapped="${DLL_LICENSE_FILES[${key}]:-}"
    if [ -z "${mapped}" ]; then
        echo "error: bundled DLL '${name}' has no known license/attribution mapping." >&2
        echo "       Refusing to ship an unaudited third-party dependency silently." >&2
        echo "       Add it to DLL_LICENSE_FILES in this script (with the correct" >&2
        echo "       upstream license file(s) staged under third_party/licenses/)" >&2
        echo "       and to THIRD_PARTY_NOTICES.md before packaging can include it." >&2
        exit 1
    fi
    for f in ${mapped}; do
        LEGAL_FILES_NEEDED+=("${f}")
    done
done
echo "  legal material required for this build: ${LEGAL_FILES_NEEDED[*]}"

echo "==> Copying curated release examples"
# WINDOWS PORTABLE PACKAGE M2 spec §2: same curated set as the Linux
# release (examples/README.md) - each compiled AND RUN against the
# just-installed release kaicc.exe, exact stdout asserted, LF-only (no
# CRLF normalization - see runtime/kai_runtime.c's WINDOWS M1.1 fix).
mkdir -p "${DEST}/examples"

check_release_example() {
    local name="$1"
    local expected="$2"
    cp "${REPO_ROOT}/examples/${name}" "${DEST}/examples/${name}"
    local out_exe="${BUILD_DIR}/release_example_check.exe"
    rm -f "${out_exe}"
    "${KAICC_EXE}" "${REPO_ROOT}/examples/${name}" -o "${out_exe}"
    local actual
    actual="$("${out_exe}")"
    if [ "${actual}" != "${expected}" ]; then
        echo "error: release example ${name} did not produce the expected stdout" >&2
        printf "  expected: %s\n" "${expected}" >&2
        printf "  actual:   %s\n" "${actual}" >&2
        exit 1
    fi
    echo "  ${name}: OK"
    rm -f "${out_exe}"
}

echo "Verifying curated release examples (from the installed staging tree)..."
check_release_example "hello.kai" "$(printf "Hello from KAI")"
check_release_example "functions.kai" "$(printf "Hello\nKAI\n42\n84")"
check_release_example "conditions.kai" "$(printf "adult\npositive\nnegative\nzero")"
check_release_example "variables.kai" "$(printf "KAI\n0.1\n2026\n1")"

echo "==> Verifying semantic tooling from the installed staging tree (WINDOWS"
echo "    PORTABLE PACKAGE M2 spec §19 - proves the public binary contains"
echo "    more than the compile path)"
inspect_out="$("${KAICC_EXE}" inspect "${DEST}/examples/functions.kai" --json)"
echo "${inspect_out}" | grep -q '"kind":"function"' || {
    echo "error: kaicc.exe inspect did not report any function symbol" >&2
    exit 1
}
echo "  inspect: OK"

# `add(20, 22)` is called from functions.kai line 15, columns 13-15 -
# column 13 is the call-site identifier's first character.
references_out="$("${KAICC_EXE}" references "${DEST}/examples/functions.kai" --line 15 --column 13 --json)"
echo "${references_out}" | grep -q '"line"' || {
    echo "error: kaicc.exe references did not report any reference location" >&2
    exit 1
}
echo "  references: OK"

call_graph_out="$("${KAICC_EXE}" call-graph "${DEST}/examples/functions.kai" --json)"
echo "${call_graph_out}" | grep -q '"add"' || {
    echo "error: kaicc.exe call-graph did not mention the 'add' function" >&2
    exit 1
}
echo "  call-graph: OK"

echo "==> Copying legal/documentation files"
cp "${REPO_ROOT}/LICENSE" "${DEST}/LICENSE"
cp "${REPO_ROOT}/README.md" "${DEST}/README.md"
cp "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" "${DEST}/THIRD_PARTY_NOTICES.md"
mkdir -p "${DEST}/third_party/licenses"
# WINDOWS PORTABLE PACKAGE M2.1 spec §2/§11: copy ONLY the license files
# this specific Windows build actually needs (LEGAL_FILES_NEEDED, computed
# above from the real DLL closure) - deliberately NOT a blanket `cp *.txt`
# of the whole third_party/licenses/ directory, since that directory also
# holds Z3's license material for the Linux release, and the Windows
# package must never claim to bundle Z3 (it doesn't - see
# THIRD_PARTY_NOTICES.md's Z3 section).
declare -A _legal_file_seen=()
for f in "${LEGAL_FILES_NEEDED[@]}"; do
    if [ -n "${_legal_file_seen[${f}]+x}" ]; then
        continue
    fi
    _legal_file_seen[${f}]=1
    src="${REPO_ROOT}/third_party/licenses/${f}"
    if [ ! -f "${src}" ]; then
        echo "error: required license file missing from source tree: third_party/licenses/${f}" >&2
        echo "       (needed for one of the DLLs this build actually bundles - see" >&2
        echo "       the dependency manifest above)" >&2
        exit 1
    fi
    cp "${src}" "${DEST}/third_party/licenses/${f}"
done

echo "==> Verifying required release documentation/notice files are present"
# WINDOWS PORTABLE PACKAGE M2.1 spec §18: verify presence IN THE STAGED
# ARTIFACT itself, not just that the source tree had the file - this is
# what actually ends up in the ZIP.
REQUIRED_FILES=(
    "LICENSE"
    "README.md"
    "THIRD_PARTY_NOTICES.md"
)
for f in "${LEGAL_FILES_NEEDED[@]}"; do
    REQUIRED_FILES+=("third_party/licenses/${f}")
done
for f in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "${DEST}/${f}" ]; then
        echo "error: required release file missing: ${f}" >&2
        exit 1
    fi
    echo "  ${f}: present"
done

echo "==> Portable artifact staged at dist/${ARTIFACT_NAME}/"
find "${DEST}" -type f

KAICC_VERSION="$("${KAICC_EXE}" --version | awk '{print $2}')"
if [ -z "${KAICC_VERSION}" ]; then
    echo "error: could not determine kaicc version from '${KAICC_EXE} --version'" >&2
    exit 1
fi
echo "==> Release kaicc.exe version: ${KAICC_VERSION}"

VERSIONED_ARCHIVE_NAME="kai-${KAICC_VERSION}-windows-x86_64.zip"
ARCHIVE_PATH="${DIST_DIR}/${VERSIONED_ARCHIVE_NAME}"

echo "==> Producing dist/${VERSIONED_ARCHIVE_NAME}"
rm -f "${ARCHIVE_PATH}"
(cd "${DIST_DIR}" && zip -r -q "${VERSIONED_ARCHIVE_NAME}" "${ARTIFACT_NAME}")
echo "    $(du -h "${ARCHIVE_PATH}" | cut -f1)"

echo "==> Generating SHA-256 (informational - not the official release SHA256SUMS)"
(cd "${DIST_DIR}" && sha256sum "${VERSIONED_ARCHIVE_NAME}" | tee "${VERSIONED_ARCHIVE_NAME}.sha256")

echo "==> Done."
