# Third-Party Notices

This file documents third-party software incorporated into or bundled with
the KAI-CC **binary release artifacts** (`kaicc` and the files staged
alongside it in a portable release layout, e.g. `dist/kai-linux-x86_64/`).
It does not cover the KAI-CC source repository's own build-time or
development-only dependencies (CMake, Ninja, the LLVM *development*
headers used only to compile KAI-CC itself, etc.) - only what actually
ends up inside a distributed binary.

This file is factual notice preparation, not a legal opinion. It does not
constitute legal advice, and it does not itself grant any rights to KAI-CC
or its own source code - see "Project License" below and the root
`README.md`.

## LLVM Project

**How it is incorporated:** `kaicc` statically links the LLVM 22
components it uses for code generation (see `CMakeLists.txt`'s
`llvm_map_components_to_libnames` call: `core`, `native`, `nativecodegen`,
`mc`, `object`, `support`, `target`). No LLVM shared library
(`libLLVM*.so`) is a runtime dependency of any KAI-CC release build -
confirmed directly via `ldd` on the release artifact (see the Release
Hardening M1/M1.1 audits).

**License:** Apache License v2.0 WITH LLVM Exceptions (the LLVM Project
also offers portions under the University of Illinois/NCSA license; the
canonical modern LLVM license is the Apache-2.0-with-exceptions text
below).

**License text source:** copied verbatim from
`/usr/share/licenses/llvm-libs/LICENSE.TXT`, as shipped by the Fedora
`llvm-libs` package (LLVM 22.1.8) - the exact upstream LLVM Project
license text, unmodified. See
[`third_party/licenses/LLVM-LICENSE.txt`](third_party/licenses/LLVM-LICENSE.txt).

## Z3 (Microsoft Research)

**How it is incorporated:** the portable Linux x86_64 release build
(Ubuntu 22.04 baseline, `release/linux-x86_64/Containerfile`) links
against an LLVM 22 package that was built with Z3 SMT-solver support
enabled (`LLVM_ENABLE_Z3_SOLVER=ON`), which makes `LLVMSupport`
unconditionally depend on `libz3.so.4` even though KAI-CC never calls any
Z3 API itself (see the Release Hardening M1.1 audit for the exact
investigation). Because no static Z3 library is available from Ubuntu's
package repositories, the release artifact **bundles the Z3 shared
library itself**
(`dist/kai-linux-x86_64/lib/kai/libz3.so.4`) alongside `kaicc`, located via
an origin-relative RPATH (`$ORIGIN/../lib/kai`). Any release/VSIX built
from this Ubuntu 22.04 baseline therefore redistributes an unmodified
binary copy of Z3.

**License:** MIT ("Expat").

**License text source:** copied verbatim from
`/usr/share/doc/libz3-4/copyright` inside the Ubuntu 22.04 release-build
container (Debian packaging copyright-file format, `libz3-4` package,
Z3 4.8.12) - this is the Debian package's copyright/license metadata
file, which embeds the actual MIT license text and copyright holders
(Microsoft Corporation and named contributors) inline; it is not Z3's own
repository-root `LICENSE.txt` file, though the license terms are the
same. See
[`third_party/licenses/Z3-COPYRIGHT.txt`](third_party/licenses/Z3-COPYRIGHT.txt).

## What still needs review before public binary distribution

- Confirm whether Z3's own upstream repository `LICENSE.txt`
  (https://github.com/Z3Prover/z3) should be sourced directly instead of
  (or in addition to) the Debian packaging copyright file captured here -
  both state the same MIT terms, but the upstream file is the more
  conventional citation for a project's own license text.
- Confirm the exact LLVM Project copyright/attribution line expected for
  a binary redistribution under the Apache-2.0-with-exceptions terms
  (the captured license text already includes the standard LLVM Project
  notice).
- This document intentionally does not make any determination about
  compatibility between these third-party licenses and KAI-CC's own
  (currently unselected) project license - see "Project License" below.

## Project License

KAI-CC's own source code does not yet have a selected license - see the
root `README.md`. The third-party notices above describe material
KAI-CC's binary releases incorporate; they are independent of, and do not
resolve, that still-open decision.
