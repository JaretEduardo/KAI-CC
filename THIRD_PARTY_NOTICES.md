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

**License text source:** the exact upstream Z3 `LICENSE.txt`, fetched
directly from the official Z3Prover/z3 GitHub repository
(https://github.com/Z3Prover/z3) pinned to tag `z3-4.8.12` -
https://raw.githubusercontent.com/Z3Prover/z3/z3-4.8.12/LICENSE.txt. This
tag was confirmed (via Launchpad's Ubuntu archive API for the `jammy`
series) to be the exact upstream version corresponding to the `z3`
source package (`4.8.12-1`) that produces Ubuntu 22.04's `libz3-4`
binary package bundled in the release artifact - not an assumed or
approximate version. See
[`third_party/licenses/Z3-LICENSE.txt`](third_party/licenses/Z3-LICENSE.txt).

Additionally,
[`third_party/licenses/Z3-COPYRIGHT.txt`](third_party/licenses/Z3-COPYRIGHT.txt)
(the Debian packaging copyright file, previously the only Z3 legal file
tracked here) is kept alongside it: it lists upstream Z3 copyright
holders beyond Microsoft Corporation (Arie Gurfinkel, Saint-Petersburg
State University, Matteo Marescotti) that the bare upstream `LICENSE.txt`
does not itself enumerate, so it remains useful for complete attribution.
Its `Files: debian/*` section (GPL-2+) describes only the Debian
packaging scripts, not any code that ends up inside the redistributed
`libz3.so.4` binary, and does not apply to KAI-CC's release artifact.

## Upstream NOTICE files

Neither the LLVM Project (at the LLVM 22 release line packaged by
Fedora's `llvm-libs`/Ubuntu's `llvm-22-dev`) nor Z3 (at tag `z3-4.8.12`)
publishes a repository-root `NOTICE` file requiring separate
reproduction alongside their license text - checked directly against
each project's repository contents at the relevant version. No
additional upstream NOTICE material applies here.

## What still needs review before public binary distribution

- Confirm the exact LLVM Project copyright/attribution line expected for
  a binary redistribution under the Apache-2.0-with-exceptions terms
  (the captured license text already includes the standard LLVM Project
  notice).
- The release Containerfile pins the apt.llvm.org component
  (`llvm-toolchain-jammy-22`), which tracks the latest LLVM 22.x build
  rather than one exact patch release; the Apache-2.0-with-exceptions
  license text is unchanged across LLVM 22.x patch releases, so this does
  not affect the accuracy of the tracked license text, but a specific
  patch version should be confirmed against whichever build actually
  produced a given release artifact (see that release's own build log).

## Project License

KAI-CC's own source code is licensed under the Apache License, Version
2.0 - see the root [`LICENSE`](LICENSE) and `README.md`. The
third-party notices above describe material KAI-CC's binary releases
incorporate under their own separate licenses; they are independent of
KAI-CC's own project license.
