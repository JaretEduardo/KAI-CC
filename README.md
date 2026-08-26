# KAI

**KAI** is an experimental, statically-typed, compiled systems programming language explored with AI-assisted
software development in mind from the start. **KAI-CC** is its reference compiler.

> **Status: early / pre-release.** KAI-CC currently targets **Linux x86_64 only**. It compiles a real (but
> still small) subset of KAI source directly to a native executable via LLVM. Syntax, semantics, and tooling
> may change significantly. Do not use KAI for production software.

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
explicit `return`.

**Text:** string literals, explicit `str` local annotations, `str` function parameters and return types, and
`print(str)`. `str` values (including those containing an embedded `\0` byte, which is valid UTF-8) are
preserved byte-for-byte end to end, since KAI's runtime tracks byte length explicitly rather than relying on
C-string termination. The target invariant is that `str` always holds valid UTF-8, but the compiler does not
yet enforce this by validating string contents - it preserves whatever bytes the source file contained.

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
**Build Current File** / **Run Current File** commands, with a bundled Linux x86_64 `kaicc` + runtime when
packaged as a VSIX. See [`editors/vscode/README.md`](editors/vscode/README.md) for full extension
documentation, and "Portable Linux release" below for how that bundled compiler is produced
(`KAI_RELEASE_ROOT` is a packaging-time detail for whoever builds the VSIX, not something an extension user
needs to set).

---

## Building from source

Required:

- CMake >= 3.20
- Ninja (or another CMake-supported generator)
- a C++23 compiler
- LLVM 22 development packages
- a working host C compiler driver (`cc`, `clang`, or `gcc`) for the final native link step

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
into - see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)). End users **do** need a host C compiler driver
(`cc`/`clang`/`gcc`) on `PATH`, since `kaicc` shells out to it for the final native link of every program it
compiles.

This artifact is built from an Ubuntu 22.04 baseline and has been tested running on both Ubuntu 22.04 and a
current Fedora host. This is **not** a claim of universal Linux compatibility - only what has actually been
verified.

---

## Examples

See [`examples/README.md`](examples/README.md) for the exact status of every tracked example, including which
ones are verified end-to-end, which one is an intentionally-invalid diagnostics demo, and which are
design/future sketches that don't compile with the current backend yet.

---

## Current limitations

Parses and/or type-checks in some form, but explicitly **not** backend-lowerable yet:

- `for` loops (LLVM code generation is not implemented)
- arrays/slices as function parameter, return, or local types
- structs, enums, generics, traits
- `Result`, general references (`&T`), and advanced ownership/borrowing
- an owned, dynamic `String` type (`str` today is a `Copy`, immutable, non-owning view only)
- string concatenation, equality, slicing, and indexing
- modules/packages

Platform: **Linux x86_64 only**. No Windows or macOS support exists or is claimed.

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

**License: not yet selected.** This repository does not currently grant any license to use, copy, modify, or
redistribute its own source code; that decision has not been made yet.

Binary releases incorporate third-party software (LLVM, Z3) under their own separate licenses - see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

## Origins

KAI-CC is a ground-up redesign inspired by lessons learned while developing an earlier **Kaii** compiler
prototype (lexing, parsing, AST construction, semantic analysis, type checking, C code generation). KAI-CC
takes a new direction focused on AI-native language design, native compilation, semantic tooling, and
long-term compiler architecture.
