# KAI

[![CI](https://github.com/JaretEduardo/KAI-CC/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/JaretEduardo/KAI-CC/actions/workflows/ci.yml)

**KAI** is an experimental, statically-typed, compiled systems programming language explored with AI-assisted
software development in mind from the start. **KAI-CC** is its reference compiler.

> **Status: alpha / pre-release (v0.1.0-alpha.2).** KAI-CC targets **Linux x86_64 and Windows x86_64**. It
> compiles a real (but still small) subset of KAI source directly to a native executable via LLVM. Syntax,
> semantics, and tooling may change significantly. Do not use KAI for production software.

---

## Current status

KAI-CC has a working, end-to-end MVP pipeline: `.kai` source goes through a real lexer, parser, semantic
analyzer, type checker, and control-flow checker, then straight to LLVM IR, native object code, and a linked
executable - all invoked with one command:

```
kaicc <file.kai> -o <output>
```

This is **current implementation status**, not the long-term language design. See "What works today" and
"Current limitations" below for the exact boundary, and the [design documents](#documentation) for where the
language is headed - those documents describe intent and direction, not necessarily what compiles today.

---

## Quickstart

Build `kaicc` (see "Building from source" below), then:

```kai
// hello.kai
fn main() {
    print("Hello from KAI")
}
```

```sh
./build/bin/kaicc examples/hello.kai -o hello
./hello
```

```
Hello from KAI
```

KAI is more than literal printing - functions, parameters, and `str` values work too:

```kai
fn greet(name: str) {
    print(name)
}

fn language() -> str {
    return "KAI"
}

fn main() {
    greet("Hello")
    print(language())
}
```

compiles and runs the same way, printing `Hello` then `KAI`.

---

## What works today

**Types:** `i8`/`i16`/`i32`/`i64`, `u8`/`u16`/`u32`/`u64`, `f32`/`f64`, `bool`, `char`, the unit type `()`, and
`str` (an immutable, non-owning text view - see "Current limitations" for what `str` does *not* yet include).

**Expressions/statements:** literals, `let` (immutable) and `mut` (mutable) bindings, arithmetic, comparisons,
logical `&&`/`||` (short-circuit), assignment, function calls, recursion, `if`/`else if`/`else`, `while`,
`for i in start..end` over a half-open integer range (KAI LANGUAGE M6, post-alpha.2 - see "Current limitations"
below for exactly what `for` does *not* yet cover), explicit `return`.

**Text:** string literals, explicit `str` local annotations, `str` function parameters and return types, and
`print(str)`. `str` values (including those containing an embedded `\0` byte, which is valid UTF-8) are
preserved byte-for-byte end to end, since KAI's runtime tracks byte length explicitly rather than relying on
C-string termination. The target invariant is that `str` always holds valid UTF-8, but the compiler does not
yet enforce this by validating string contents - it preserves whatever bytes the source file contained.

**Output byte semantics (WINDOWS M1.1):** every `print` call's line terminator is exactly one LF byte (`0x0A`),
identically on every supported platform, including Windows - never the host-native line ending. The KAI runtime
(`runtime/kai_runtime.c`) explicitly puts Windows' stdout into binary mode before writing, since Windows' C
runtime otherwise silently rewrites `LF` to `CRLF` on output. This is a deliberate, permanent KAI contract, not
an incidental detail of the current implementation: deterministic byte-identical output across platforms is
easier to test and easier for tools/agents to consume reliably.

**Semantic tooling:** `kaicc` can answer structural questions about a `.kai` file directly from its own
resolved semantic model - see "Semantic tooling" below.

---

## Semantic tooling

KAI-CC exposes its own semantic model directly, rather than requiring a tool (or an AI agent) to reconstruct
symbol/type/call relationships from text search:

```sh
./build/bin/kaicc inspect examples/functions.kai --json
```

```
kaicc definition <file.kai> --line N --column M --json
kaicc references <file.kai> --line N --column M --json
kaicc callers <file.kai> --line N --column M --json
kaicc callees <file.kai> --line N --column M --json
kaicc call-graph <file.kai> --json
```

`definition`/`references`/`callers`/`callees` resolve the symbol at a given 1-indexed `--line`/`--column`;
`inspect` and `call-graph` operate on the whole file. All of these are real compiler-resolved queries -
correctly distinguishing shadowed locals or a user function that shadows a builtin, for example - not textual
grep. See [`docs/CLI.md`](docs/CLI.md) for the full reference.

---

## VS Code extension

`editors/vscode` provides syntax highlighting, snippets, basic completion, KAI file icons/branding, and
**Build Current File** / **Run Current File** commands. It ships as two separate, platform-specific VSIX
packages - Linux x64 and Windows x64 - each bundling that platform's native `kaicc`/`kaicc.exe` + runtime by
default; a single VSIX never bundles both platforms' compilers, and your platform's package installs the
right one automatically. See [`editors/vscode/README.md`](editors/vscode/README.md) for full extension
documentation, and "Portable Linux release"/"Portable Windows release" below for how each bundled compiler is
produced (`KAI_RELEASE_ROOT` is a packaging-time detail for whoever builds the VSIX, not something an
extension user needs to set).

---

## Building from source

Required:

- CMake >= 3.20
- Ninja (or another CMake-supported generator)
- a C++23 compiler
- LLVM 22 development packages
- a working host C toolchain for the final native link step: a `cc`/`clang`/`gcc`-compatible compiler
  driver, plus the platform's normal libc development/startup files and linker support (a bare compiler
  driver package is not always sufficient by itself - see "Portable Linux release" below)

Package names vary by distribution; the following is what this repository is actually developed and tested
against (**Fedora-specific** - not a claim that other distributions use the same names):

```sh
sudo dnf install cmake ninja-build gcc-c++ llvm-devel llvm-static
```

Then, from the repository root:

```sh
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces `build/bin/kaicc` and `build/lib/kai/libkai_runtime.a`. No Ubuntu-host-native package
instructions are given here since they haven't been verified the same way - see "Portable Linux release"
below for the separate, containerized way this project produces a portable Ubuntu-22.04-baseline build.

Pull requests and `master` are validated by GitHub Actions using this same portable Linux x86_64 baseline -
see [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

---

## Portable Linux release

A locally-built `kaicc` is tied to whatever glibc/libstdc++ your own machine has. This repository also
produces a **portable** release artifact from an older, more broadly-compatible baseline:

```sh
scripts/build-release-linux-x86_64.sh
```

This uses Podman (or Docker as a fallback) to build KAI-CC inside an Ubuntu 22.04 container
(`release/linux-x86_64/Containerfile`), runs the full test suite inside it, and produces:

```
dist/kai-linux-x86_64/
dist/kai-linux-x86_64.tar.gz
```

with the layout:

```
kai-linux-x86_64/
  bin/kaicc
  lib/kai/libkai_runtime.a
  lib/kai/libz3.so.4
  examples/*.kai        (curated, known-good examples only)
```

**End users do not need LLVM installed** - `kaicc` statically links the LLVM components it needs.
`libz3.so.4` is bundled for the same reason (an upstream LLVM packaging dependency KAI-CC itself never calls
into - see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)). Native compilation still requires a working
host C toolchain: a `cc`/`clang`/`gcc`-compatible compiler driver on `PATH`, since `kaicc` shells out to it
for the final native link of every program it compiles, **plus** the platform's normal libc
development/startup files and linker support - a bare compiler driver package is not always enough by itself.
For example, a minimal Ubuntu 22.04 install needs both `gcc` and `libc6-dev` (installing `gcc` alone,
without its recommended packages, was confirmed to fail linking with a missing `Scrt1.o`/`crti.o` error);
most desktop Linux installs and `build-essential`-style metapackages already include everything needed.

This artifact is built from an Ubuntu 22.04 baseline and has been tested running on both Ubuntu 22.04 and a
current Fedora host. This is **not** a claim of universal Linux compatibility - only what has actually been
verified.

---

## Portable Windows release

A portable Windows x86_64 distribution, analogous in spirit to the Linux release above, is built from an
MSYS2 UCRT64 baseline (the same one the `compiler-windows` CI job already builds/tests against):

```sh
scripts/build-release-windows-x86_64.sh
```

Run from inside an MSYS2 UCRT64 shell (never WSL, never a plain `cmd.exe`/PowerShell prompt), this configures a
Release build, runs the full CTest suite, installs to a staging tree, discovers and bundles `kaicc.exe`'s actual
recursive non-system DLL dependencies (never guessed - see the script's own dependency-manifest output), verifies
every bundled DLL has a known license/attribution mapping (failing the build rather than shipping an unaudited
dependency - see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)), copies the same curated examples as the
Linux release, and produces:

```
dist/kai-windows-x86_64/
dist/kai-<version>-windows-x86_64.zip
```

with the layout:

```
kai-windows-x86_64/
  bin/kaicc.exe
  bin/libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll, zlib1.dll, libzstd.dll
  lib/kai/libkai_runtime.a
  examples/*.kai        (same curated set as the Linux release)
```

The five bundled DLLs are MSYS2 UCRT64 runtime dependencies of `kaicc.exe` (GCC's runtime, mingw-w64's
winpthreads, zlib, and zstd) - confirmed empirically from the built binary's own PE import table, not assumed.
`kaicc.exe` does **not** depend on a separate LLVM or Z3 DLL - LLVM object code is statically incorporated into
`kaicc.exe` itself (same as the Linux release), and Z3 is not linked at all on this platform. This closure can
change if the underlying MSYS2 toolchain packages change; the packaging script re-discovers and re-audits it on
every run rather than hard-coding this list.

`kaicc.exe` itself needs no separate LLVM/MSYS2 installation to start or to answer semantic queries
(`inspect`/`references`/`call-graph`/...) - the bundled DLLs above ship in `bin/` alongside it, and Windows' own
executable-directory DLL search resolves them with no `PATH` changes. **Native compilation still requires a
working host C toolchain on `PATH`** (a `clang`/`gcc`-compatible driver `kaicc.exe` shells out to for the final
link step), exactly as the Linux release requires `gcc`/`clang` + `libc6-dev` - this is a real, documented
remaining requirement for this alpha, not an oversight.

This packaging, plus a from-scratch "download -> extract -> compile -> run" smoke test using an independently
obtained host toolchain (no KAI/MSYS2 build tooling present), is validated end to end by CI on every commit.
**`v0.1.0-alpha.2` release artifacts will include this `.zip`** once published; until then it is CI build
evidence, not yet an attached GitHub Release asset.

### Windows quickstart (for someone who has never touched this repository)

This is the exact, minimal sequence a Windows x86_64 user follows once `v0.1.0-alpha.2` release artifacts are
published - **no MSYS2, no LLVM, no CMake/Ninja, no source build** is needed for any of it:

1. Download and extract `kai-<version>-windows-x86_64.zip` anywhere (an ordinary `Downloads` folder is fine -
   this works even from a path containing spaces, e.g. `C:\Users\Jane Doe\Downloads\kai-windows-x86_64\`).
2. Install a host C toolchain - **only needed to compile KAI programs, not to run `kaicc.exe` itself.** The
   one route this project actually tests is
   [WinLibs GCC](https://github.com/brechtsanders/winlibs_mingw) (a standalone build of GCC/MinGW-w64,
   distributed as a plain `.zip` - no installer): download the UCRT runtime, `x86_64`, `.zip` variant, and
   **extract it to a directory whose path does not contain spaces** (e.g. `C:\Tools\winlibs`) - this specific
   toolchain distribution does not tolerate being relocated into a space-containing install prefix (confirmed
   directly: its own linker fails there). This is a limitation of that toolchain, not of KAI - KAI itself, your
   `.kai` source files, and your compiled output may all live under a path containing spaces without issue (see
   step 1). Add the extracted `mingw64\bin` folder to your `PATH` (or set `KAI_CC` to the full path of
   `gcc.exe` inside it - see `docs/CLI.md`). This project does not yet test any other route (Visual
   Studio/MSVC is out of scope for this alpha); advanced users may try another Clang/GCC-compatible driver on
   `PATH`/`KAI_CC` at their own risk.
3. Open PowerShell (or Command Prompt) in the extracted `kai-windows-x86_64\` folder.
4. `.\bin\kaicc.exe --version` - this alone works with **no toolchain installed at all**; so do
   `inspect`/`references`/`call-graph` (see `docs/CLI.md`) - only the final native link step needs the
   toolchain from step 2.
5. `.\bin\kaicc.exe .\examples\hello.kai -o hello` produces `hello.exe`.
6. `.\hello.exe` prints `Hello from KAI`.

If step 2 is skipped, step 5 fails with a clear `no usable host C compiler driver found` error (exit code 9,
see `docs/CLI.md`) rather than a confusing crash or DLL error - this is expected, not a bug.

---

## Examples

See [`examples/README.md`](examples/README.md) for the exact status of every tracked example, including which
ones are verified end-to-end, which one is an intentionally-invalid diagnostics demo, and which are
design/future sketches that don't compile with the current backend yet.

---

## Current limitations

`for i in start..end` over a half-open integer range is real, native, executable code (KAI LANGUAGE M6,
post-alpha.2 - both endpoints must be the same concrete integer type, following the same contextual-literal-
adaptation rules as arithmetic; no implicit widening/narrowing). KAI 0.1 still has no general iteration
protocol, though: `for x in someCollection` over anything other than a literal `start..end` range is rejected
as a semantic error, not silently ignored.

A fixed-size array `[T; N]` is a real semantic type as of KAI LANGUAGE M7A (post-alpha.2): `let xs = [1, 2, 3]`
and an explicitly-annotated `let xs: [i32; 3] = [1, 2, 3]` both resolve and type-check (homogeneous elements
only, using the same contextual-literal-adaptation rules as arithmetic), and `kaicc inspect` renders the type
canonically as `[i32; 3]`. This is frontend type-system work only - **no array program compiles to a native
executable yet**: element reads/writes, bounds checking, and LLVM lowering itself all remain unimplemented, so
an array-typed local/parameter/return still fails cleanly at (or before) code generation, never silently
miscompiling. Also parses and/or type-checks in some form, but explicitly **not** backend-lowerable yet:

- native execution of arrays (element access, bounds checking, LLVM lowering - see above), and slices (`[T]`)
  as a semantic type at all
- structs, enums, generics, traits
- `Result`, general references (`&T`), and advanced ownership/borrowing
- an owned, dynamic `String` type (`str` today is a `Copy`, immutable, non-owning view only)
- string concatenation, equality, slicing, and indexing
- modules/packages

Platform: **Linux x86_64 and Windows x86_64** for the released/distributed compiler. No macOS or ARM64 (any
OS) support exists or is claimed.

Windows support (MSYS2 UCRT64 build baseline, Clang/LLVM 22.1.8 - see the `compiler-windows` CI job and
"Portable Windows release" above) is CI-proven end to end on every commit: the same source tree builds
`kaicc.exe`, runs its full CTest suite, produces a portable `.zip` distribution, packages a platform-specific
VS Code VSIX, and passes an independent fresh-user smoke test proving an ordinary Windows user - with none of
KAI's own MSYS2/LLVM/CMake/Ninja build tooling installed - can download the released ZIP and VSIX and
compile/run real KAI programs with only a separately-obtained host C toolchain. As on Linux, compiling a KAI
program still requires that host C toolchain on `PATH` (see "Portable Windows release" above) - a fully
toolchain-free experience is not claimed on either platform. `v0.1.0-alpha.2` is the first release to include
Windows binary artifacts once published.

---

## Documentation

- [Language Design](LANGUAGE_DESIGN.md), [Syntax](SYNTAX.md), [Grammar](GRAMMAR.md) - language direction, not
  all of it implemented yet
- [Type System](TYPE_SYSTEM.md), [Memory Model](MEMORY_MODEL.md) - including the current `str`/`String`
  design and its own current-implementation-status notes
- [Compiler Architecture](COMPILER_ARCHITECTURE.md) - the actual current pipeline (Lexer -> Parser/AST ->
  SemanticAnalyzer -> TypeChecker -> ControlFlowAnalyzer -> LLVM IR directly from AST + the resolved semantic
  model, with HIR intentionally postponed - not the illustrative "with HIR" diagram some design docs still
  show for the eventual, longer-term architecture)
- [Roadmap](ROADMAP.md)
- [CLI Reference](docs/CLI.md) - commands, flags, exit codes

---

## License

KAI-CC is licensed under the **Apache License, Version 2.0** - see [`LICENSE`](LICENSE) for the full text.

Binary releases incorporate third-party software (LLVM, Z3) under their own separate licenses - see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

## Origins

KAI-CC is a ground-up redesign inspired by lessons learned while developing an earlier **Kaii** compiler
prototype (lexing, parsing, AST construction, semantic analysis, type checking, C code generation). KAI-CC
takes a new direction focused on AI-native language design, native compilation, semantic tooling, and
long-term compiler architecture.
