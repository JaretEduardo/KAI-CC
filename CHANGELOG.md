# Changelog

All notable KAI-CC releases are documented here. See
[`docs/releases/`](docs/releases/) for the full notes of each release.

## 0.1.0-alpha.3

A language-feature and community-readiness release. Adds half-open integer range `for` loops (M6); fixed-size
arrays `[T; N]` with checked compile-time and runtime bounds, whole-array value semantics, and array function
parameters/returns lowered as a direct LLVM aggregate (M7-M8); general nested fixed-array indexing at any
depth (M9); immutable, non-owning slice values `[T]` with `len()`, checked indexing, and a slice function
parameter (M10); and a restricted, flow-sensitive slice provenance analysis (`External`/`Local`/`Unknown`)
that proves a narrow, sound class of slice returns safe and makes exactly that class executable via a direct
`{ptr, len}` aggregate return - never a general borrow checker, interprocedural inference, or mutable/composite
slices (M11). The curated release examples now include `loops.kai`, `fibonacci.kai`, `arrays.kai`, and
`slices.kai` alongside the existing basics, demonstrating all of the above end to end. Adds `CONTRIBUTING.md`
and issue templates, fixes a stale platform-support claim in `COMPILER_ARCHITECTURE.md`, and improves
README.md's newcomer path (a Linux prebuilt-release quickstart, a concrete grounding of the "AI-native" claim,
and GitHub Releases/Issues links). The VS Code extension is bumped to `0.7.0` to distinguish its newer bundled
compiler from the previously published `0.6.0` VSIX; no extension functionality changed. No language-
architecture changes, no benchmark trials (`textual: 0, semantic: 0` unchanged). See
[`docs/releases/0.1.0-alpha.3.md`](docs/releases/0.1.0-alpha.3.md) for full details, including known
limitations.

## 0.1.0-alpha.2

Adds native **Windows x86_64** support alongside the existing Linux x86_64
compiler, as a real, CI-proven, second target platform - not a design
sketch. `kaicc.exe` builds, runs its full CTest suite, and compiles/links
real `.kai` programs to native `.exe` executables on Windows, with
deterministic LF-only `print` output identical to Linux. Adds a portable
Windows x86_64 `.zip` distribution (recursively-audited DLL closure:
`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`,
`zlib1.dll`, `libzstd.dll` - no LLVM or Z3 DLL, both statically linked as
on Linux) with matching third-party license/notice coverage. Adds a
platform-specific Windows x64 VSIX for the VS Code extension (alongside
the existing Linux x64 VSIX), bundling `kaicc.exe` and its runtime, with
Build/Run Current File support producing native Windows executables.
Windows CI now covers compiler build/test/package, VS Code extension
packaging, and an independent "fresh user" smoke test that proves an
ordinary Windows user - with no MSYS2, LLVM, CMake, or Ninja installed,
using only a separately-downloaded host C toolchain (WinLibs GCC) - can
download the released ZIP and VSIX and compile/run real KAI programs.
Semantic tooling (`inspect`/`definition`/`references`/`callers`/
`callees`/`call-graph`) is unaffected and remains available without a
host toolchain on both platforms. No language/compiler-semantics changes
in this release; formal AI-native benchmark trial counts remain zero
(infrastructure exists, no trials have been run). See
[`docs/releases/0.1.0-alpha.2.md`](docs/releases/0.1.0-alpha.2.md) for
full details, including known limitations.

## 0.1.0-alpha.1

First public pre-release of the KAI reference compiler. A complete,
working, end-to-end pipeline (lexer, parser, semantic analysis, type
checking, control-flow analysis, LLVM codegen, native object emission and
linking) compiles a real subset of KAI source to a native Linux x86_64
executable. Includes spellable `str` parameters/returns, compiler-resolved
semantic tooling (`inspect`/`definition`/`references`/`callers`/`callees`/
`call-graph`), a portable Linux x86_64 binary release, and a VS Code
extension. Licensed under Apache License 2.0. See
[`docs/releases/0.1.0-alpha.1.md`](docs/releases/0.1.0-alpha.1.md) for
full details, including known limitations.
