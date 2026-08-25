# KAI-CC

**KAI-CC** is the reference compiler for **KAI**, an experimental AI-native systems programming language.

> KAI has a working MVP compiler (frontend + LLVM-based native codegen) verified on Fedora Linux x86_64; the
> language design itself is still evolving. See "Current Status" below.

---

## What is KAI?

KAI explores what a compiled systems programming language could look like if AI-assisted software development were considered from the beginning.

The goal is not simply to create shorter syntax.

KAI aims to make programs easier and cheaper for both humans and AI agents to understand, modify, debug, and maintain.

Core ideas include:

- statically typed programs
- native compilation
- predictable syntax
- minimal boilerplate
- structured diagnostics
- semantic introspection
- transparent memory behavior
- low source-code context overhead
- deterministic developer tooling

KAI is intended to remain readable by humans while being highly structured for machines.

---

## Example

Proposed KAI syntax:

    fn add(a: i32, b: i32) -> i32 {
        return a + b
    }

    fn main() {
        let result = add(20, 22)
        print(result)
    }

Output:

    42

The syntax is currently experimental and may change.

---

## Why KAI?

AI coding agents currently interact with programming languages that were primarily designed for human developers.

Understanding a large codebase may require reading many files, interpreting verbose diagnostics, discovering relationships through text search, and reasoning about implicit language behavior.

KAI aims to move more of that knowledge into the compiler.

Future tooling may allow queries such as:

    kai inspect User
    kai refs calculate_total
    kai deps authenticate
    kai impact User.id

Instead of requiring tools or agents to reconstruct this information manually, KAI-CC should expose it directly from the compiler's semantic model.

---

## Compiler

The first implementation of KAI-CC will be written in **C++23** and use **LLVM** for native code generation.

Initial architecture:

    KAI Source
        |
        v
      Lexer
        |
        v
      Parser
        |
        v
       AST
        |
        v
    Semantic Analysis
        |
        v
       HIR
        |
        v
     LLVM IR
        |
        v
    Native Code

Future versions may introduce MIR, KAI IR, additional backends, and a self-hosted compiler.

---

## Current Status

KAI-CC now has a working end-to-end MVP compiler (LLVM Codegen Milestone 7): it compiles a real subset of KAI
source all the way to a native executable on **Fedora Linux x86_64**, via:

    kaicc <file.kai> -o <output>

Implemented: the full frontend (lexer, parser, semantic analysis, type checking, control-flow checking), LLVM
code generation for primitive values/arithmetic/comparisons/locals/functions/calls/recursion/`if`/`while`, a
minimal `print` builtin, and native object emission + linking into a real executable.

Not yet implemented: `for` loops, arrays/strings/structs/enums/generics, ownership/borrowing, optimization, a
higher-level `kai` CLI wrapper, and non-Linux/non-x86_64 native support.

See `CLAUDE.md` and `ROADMAP.md` for the exact current implementation status.

---

## Project Goals

KAI aims to eventually provide:

- native binaries
- static type safety
- deterministic resource management
- structured compiler diagnostics
- semantic development tooling
- project and package management
- compiler introspection
- optimization infrastructure
- scalable library and framework ecosystem

Long-term research directions include:

- AI agent integration
- SIMD
- parallel computing
- GPU support
- self-hosting

---

## Design Documents

- [Language Design](LANGUAGE_DESIGN.md)
- [Syntax](SYNTAX.md)
- [Compiler Architecture](COMPILER_ARCHITECTURE.md)
- [Roadmap](ROADMAP.md)

---

## Origins

KAI-CC is a ground-up redesign inspired by lessons learned while developing the original **Kaii** compiler prototype.

Kaii explored:

- lexical analysis
- parsing
- AST construction
- semantic analysis
- type checking
- C code generation

KAI-CC takes a new direction focused on AI-native language design, native compilation, semantic tooling, and long-term compiler architecture.

---

## Status Warning

KAI is experimental.

Syntax, semantics, compiler architecture, and project structure may change significantly during early development.

Do not use KAI for production software.

---

## License

License has not yet been selected.