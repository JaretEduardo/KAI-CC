# Third-Party Notices

This file documents third-party software incorporated into or bundled with
the KAI-CC **binary release artifacts** (`kaicc`/`kaicc.exe` and the files
staged alongside it in a portable release layout, e.g.
`dist/kai-linux-x86_64/`, `dist/kai-windows-x86_64/`). It does not cover
the KAI-CC source repository's own build-time or development-only
dependencies (CMake, Ninja, the LLVM *development* headers used only to
compile KAI-CC itself, etc.) - only what actually ends up inside a
distributed binary. **The exact set of bundled third-party components
differs by platform** - each section below states precisely which
platform(s) it applies to; a component listed for one platform is not
necessarily bundled on the other.

This file is factual notice preparation, not a legal opinion. It does not
constitute legal advice, and it does not itself grant any rights to KAI-CC
or its own source code - see "Project License" below and the root
`README.md`.

## LLVM Project

**How it is incorporated (Linux x86_64 release):** `kaicc` statically links
the LLVM 22 components it uses for code generation (see `CMakeLists.txt`'s
`llvm_map_components_to_libnames` call: `core`, `native`, `nativecodegen`,
`mc`, `object`, `support`, `target`). No LLVM shared library
(`libLLVM*.so`) is a runtime dependency of any KAI-CC release build -
confirmed directly via `ldd` on the release artifact (see the Release
Hardening M1/M1.1 audits).

**Windows x86_64 portable package:** the actual recursive non-system DLL
dependency closure of `kaicc.exe`, discovered empirically by
`scripts/build-release-windows-x86_64.sh` from the built binary's own PE
import table (WINDOWS PORTABLE PACKAGE M2), contains **no `libLLVM*.dll`** -
the MSYS2 UCRT64 LLVM 22 package's `kaicc.exe` is linked the same way as
the Linux build (statically), so the Windows portable package does not
bundle a separate LLVM runtime library either. LLVM's license is still
recorded below because KAI-CC's build/link model incorporates LLVM object
code into `kaicc`/`kaicc.exe` on every platform, even though no separate
LLVM shared library ships on either platform today.

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

**How it is incorporated (Linux x86_64 release):** the portable Linux
x86_64 release build (Ubuntu 22.04 baseline,
`release/linux-x86_64/Containerfile`) links
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

**Windows x86_64 portable package:** the MSYS2 UCRT64 LLVM 22 package used
to build `kaicc.exe` was **not** built with Z3 support - the actual
recursive DLL dependency closure discovered by
`scripts/build-release-windows-x86_64.sh` contains no `libz3*.dll`. The Z3
notice above and its license files describe the Linux release only; **the
Windows portable package does not bundle Z3 in any form.**

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

## Windows x86_64 portable package

`scripts/build-release-windows-x86_64.sh` (WINDOWS PORTABLE PACKAGE M2)
discovers `kaicc.exe`'s actual recursive non-system DLL dependency closure
empirically, from the built binary's own PE import table - never assumed.
The current closure (confirmed by a real Windows CI build) is exactly
these five MSYS2 UCRT64 runtime DLLs, each bundled in `bin/` alongside
`kaicc.exe`:

| Shipped file | Upstream project | MSYS2 package (`ucrt64` repo) | License(s) | License file(s) |
|---|---|---|---|---|
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | GCC (GNU Compiler Collection) runtime | `mingw-w64-ucrt-x86_64-gcc-libs` 16.2.0-3 | GPL-3.0-or-later WITH GCC-exception-3.1 (MSYS2's package metadata additionally lists AND LGPL-2.1-or-later for other files in the same package) | [`GCC-COPYING3.txt`](third_party/licenses/GCC-COPYING3.txt), [`GCC-RUNTIME-LIBRARY-EXCEPTION.txt`](third_party/licenses/GCC-RUNTIME-LIBRARY-EXCEPTION.txt) |
| `libwinpthread-1.dll` | mingw-w64 winpthreads | `mingw-w64-ucrt-x86_64-libwinpthread` 14.0.0.r302.gd7f3c5201-1 | MIT AND BSD-3-Clause-Clear | [`WINPTHREADS-COPYING.txt`](third_party/licenses/WINPTHREADS-COPYING.txt) |
| `zlib1.dll` | zlib | `mingw-w64-ucrt-x86_64-zlib` 1.3.2-2 | Zlib | [`ZLIB-LICENSE.txt`](third_party/licenses/ZLIB-LICENSE.txt) |
| `libzstd.dll` | Zstandard (Meta Platforms) | `mingw-w64-ucrt-x86_64-zstd` 1.5.7-2 | BSD-3-Clause OR GPL-2.0-or-later (dual-licensed; KAI-CC redistributes the binary as built by MSYS2, both texts are included) | [`ZSTD-LICENSE.txt`](third_party/licenses/ZSTD-LICENSE.txt), [`ZSTD-COPYING.txt`](third_party/licenses/ZSTD-COPYING.txt) |

**License text sources:** the GCC runtime files were fetched verbatim from
the `gcc-mirror/gcc` GitHub mirror of the official GCC git repository, tag
`releases/gcc-16.2.0` (`COPYING3`, `COPYING.RUNTIME`) - matching the exact
GCC version MSYS2's `gcc-libs` 16.2.0-3 package is built from. The
winpthreads `COPYING` file was fetched verbatim from the `mingw-w64/mingw-
w64` GitHub mirror's `mingw-w64-libraries/winpthreads/COPYING` (its content
already contains both the MIT-style mingw-w64 project license and the
BSD-3-Clause-Clear-style "Lockless Inc." derived-code license MSYS2's
package metadata lists together). The zlib license text was extracted
verbatim from the `madler/zlib` GitHub repository's `README` at tag
`v1.3.2`, matching `mingw-w64-ucrt-x86_64-zlib` 1.3.2-2 exactly. The zstd
`LICENSE`/`COPYING` files were fetched verbatim from the `facebook/zstd`
GitHub repository at tag `v1.5.7`, matching `mingw-w64-ucrt-x86_64-zstd`
1.5.7-2 exactly. Package-to-DLL provenance and each package's SPDX
license identifier were confirmed directly against each package's own
page on `packages.msys2.org` (the MSYS2 project's own package database),
not assumed from naming convention alone.

**msys-2.0.dll, `libLLVM*.dll`, `libz3*.dll` are confirmed NOT bundled**
in the Windows portable package (see the LLVM/Z3 sections above and
`scripts/build-release-windows-x86_64.sh`'s own dependency-closure logic,
which hard-fails the build if `msys-2.0.dll` ever appears) - no license
material is included for them on Windows.

**Windows system DLLs are not bundled and have no notice here:** ordinary
Windows-provided components the closure references (e.g. `KERNEL32.dll`,
`ADVAPI32.dll`, `ntdll.dll`, `api-ms-win-*.dll`) are resolved by Windows
itself at runtime, never copied into the KAI-CC release archive, and are
excluded from this notice for that reason - not because they are unlicensed.

**Reproducibility note:** this bundled-DLL set is discovered from the
exact MSYS2 UCRT64 build environment at build time, and may change if
those MSYS2 packages change (e.g. a GCC runtime version bump). The
packaging script itself enforces that every bundled DLL has a known
license mapping (see its "legal material verification" step) and fails
the build rather than silently shipping an unaudited dependency - this
table should be kept in sync whenever that mapping changes, not treated
as a one-time snapshot.

## Upstream NOTICE files

Neither the LLVM Project (at the LLVM 22 release line packaged by
Fedora's `llvm-libs`/Ubuntu's `llvm-22-dev`) nor Z3 (at tag `z3-4.8.12`)
publishes a repository-root `NOTICE` file requiring separate
reproduction alongside their license text - checked directly against
each project's repository contents at the relevant version. No
additional upstream NOTICE material applies here. The same is true of
the five Windows-bundled components above: none of GCC, mingw-w64
winpthreads, zlib, or zstd publishes a separate `NOTICE` file requiring
reproduction beyond their license text itself - checked directly against
each project's repository contents at the versions named above.

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
