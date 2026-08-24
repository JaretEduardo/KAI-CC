# KAI-CC Compiler Architecture

> Status: Draft  
> Implementation language: C++23  
> Initial backend: LLVM

---

## 1. Overview

KAI-CC is the reference compiler implementation for the KAI programming language.

The compiler will initially be implemented in C++23 and use LLVM for native code generation.

The architecture should remain modular enough to support future compiler backends and eventual self-hosting.

---

## 2. Initial Compilation Pipeline

KAI 0.1:

    Source Code
        |
        v
      Lexer
        |
        v
      Tokens
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
    LLVM Codegen
        |
        v
     LLVM IR
        |
        v
      LLVM
        |
        v
    Object File
        |
        v
      Linker
        |
        v
    Native Binary

KAI 0.1 does not require MIR or a custom optimization IR.

These will be introduced after the frontend is stable.

---

## 3. Future Pipeline

Long-term direction:

    KAI Source
        |
        v
       AST
        |
        v
       HIR
        |
        v
       MIR
        |
        v
      KAI IR
        |
        +--------------------+
        |                    |
        v                    v
      LLVM IR          Future Backends
        |               GPU / WASM
        v
    Native Code

---

## 4. Lexer

Responsibilities:

- convert source text into tokens
- track source locations
- recognize keywords
- recognize identifiers
- recognize literals
- recognize operators
- report lexical errors

Every token should contain at minimum:

- token kind
- source span
- optional literal value

Possible structure:

    Token {
        kind
        span
        value
    }

---

## 5. Parser

The initial parser will use recursive descent.

Responsibilities:

- consume tokens
- validate syntactic structure
- build the AST
- report syntax errors

The parser should avoid embedding type checking or code generation logic.

---

## 6. Abstract Syntax Tree

The AST represents the source program.

Possible nodes include:

    Program
    FunctionDecl
    VariableDecl
    Block
    IfStmt
    WhileStmt
    ForStmt
    ReturnStmt
    BinaryExpr
    UnaryExpr
    CallExpr
    LiteralExpr
    IdentifierExpr

AST nodes should preserve source spans for diagnostics.

---

## 7. Semantic Analysis

Semantic analysis is responsible for:

- symbol resolution
- scope validation
- type checking
- function signature validation
- return type validation
- mutability validation

Semantic analysis runs as three separate passes over one shared semantic
model, each running to completion before the next begins:

- **SemanticAnalyzer** - scopes, symbol tables, name resolution, function
  signature collection.
- **TypeChecker** - expression types and type-compatibility checks
  (literals, operators, calls, assignment, conditions, explicit
  return-statement type compatibility).
- **ControlFlowAnalyzer** - structural control-flow properties, currently
  limited to return completeness (all-paths-return). This pass does not
  build or expose a control-flow graph; it is a narrow, purely structural
  traversal of the existing AST shape.

Implemented so far: top-level function collection, primitive/unit type resolution for function signatures and local annotations, lexical scoping with same-scope duplicate detection and nested shadowing, prelude name seeding, lexical resolution of identifier uses (undefined-name and duplicate-symbol detection), primitive literal typing (including contextual literal typing and integer literal range checking), local type inference for currently-typed expressions, annotated local compatibility checking, primitive operator type checking (unary negation/not, arithmetic, modulo, comparison, equality, logical), ordinary user-function call validation (argument-count validation, contextual argument checking against the declared signature, call result typing, and known non-callable-expression detection), assignment checking for a local variable or parameter target (binding mutability validation, right-hand-side compatibility against the target's type, and assignment-expression unit typing), if/else-if/while condition Bool validation, explicit return-statement type compatibility against the enclosing function's declared return type (including contextual return-expression checking and bare-return-as-unit compatibility), and structural return-completeness (all-paths-return) analysis via the separate ControlFlowAnalyzer pass, producing a `MissingReturn` diagnostic for a concrete non-unit-returning function whose body can structurally fall through to its end without reaching a `return`.

Not yet implemented: unreachable-code analysis, constant-condition flow reasoning (e.g. recognizing `if true` or `while true` as always executing), divergence analysis, a general control-flow graph, for-loop iterable/element type validation, member/index assignment semantics, builtin call signature validation, compound/reference semantic types, ownership/borrow checking, and first-class function/method call semantics. Semantic analysis as a whole is not yet complete.

Example errors:

- undefined variable
- duplicate symbol
- incompatible type
- invalid function arguments
- assigning to immutable value

---

## 8. HIR

HIR stands for High-Level Intermediate Representation.

The HIR removes unnecessary syntax-level information and stores resolved semantic information.

For example, an identifier in the AST may contain:

    name = "value"

while the HIR should know which symbol that identifier refers to.

HIR should eventually be useful to:

- compiler passes
- IDE tooling
- AI semantic tooling
- dependency analysis

---

## 9. Semantic Database

A long-term architectural goal is to make compiler knowledge queryable.

The compiler should eventually maintain or expose information about:

- symbols
- types
- definitions
- references
- call relationships
- module dependencies
- source locations

This semantic information may power commands such as:

    kai inspect
    kai refs
    kai deps
    kai impact

The initial compiler does not need a persistent database.

The architecture should simply avoid making semantic information inaccessible.

---

## 10. Diagnostics

Diagnostics are a first-class compiler subsystem.

Diagnostics should not be printed directly from lexer/parser/type-checker code.

Compiler components should emit structured diagnostic objects.

Possible representation:

    Diagnostic {
        code
        severity
        message
        source_span
        notes
    }

Example human output:

    error[E0102]: incompatible type
      --> src/main.kai:12:8

      expected: i32
      found: str

Future machine-readable output:

    kai check --json

---

## 11. LLVM Backend

LLVM will initially provide:

- LLVM IR generation
- optimization infrastructure
- object code generation
- architecture support

Initial targets:

- x86-64

Possible future targets:

- ARM64
- WebAssembly
- GPU architectures

KAI should not expose LLVM-specific concepts directly in normal source code.

---

## 12. Runtime

KAI should keep its runtime small.

Potential runtime responsibilities include:

- process initialization
- panic handling
- basic strings
- memory allocation interface
- printing
- platform abstractions

The exact runtime architecture will evolve with the language.

---

## 13. Standard Library

The standard library should remain separate from compiler internals.

Initial library functionality may include:

- printing
- basic strings
- basic arrays
- mathematical helpers

Future functionality may include:

- collections
- files
- networking
- concurrency
- synchronization

---

## 14. CLI Architecture

Users should primarily interact with:

    kai

Possible commands:

    kai build
    kai run
    kai check
    kai test
    kai fmt

The low-level compiler executable may remain:

    kaicc

Conceptually:

    kai
     |
     +-- build -----> kaicc
     +-- run -------> kaicc + execute
     +-- check -----> compiler frontend
     +-- fmt --------> formatter
     +-- inspect ----> semantic engine

---

## 15. Proposed Repository Layout

Initial monorepo:

    KAI-CC/
    |
    +-- compiler/
    |   +-- source/
    |   +-- lexer/
    |   +-- parser/
    |   +-- ast/
    |   +-- semantic/
    |   +-- hir/
    |   +-- codegen/
    |   +-- diagnostics/
    |
    +-- runtime/
    +-- std/
    +-- tools/
    |   +-- cli/
    |
    +-- tests/
    +-- examples/
    +-- benchmarks/
    +-- docs/
    |
    +-- README.md
    +-- LANGUAGE_DESIGN.md
    +-- SYNTAX.md
    +-- COMPILER_ARCHITECTURE.md
    +-- ROADMAP.md

`source/` holds shared source-file and source-location infrastructure
(for example `SourceManager`, `SourceLocation`, `SourceSpan`) used by the
lexer, parser, AST, semantic analysis, and diagnostics. It has no
dependency on any other compiler stage or on LLVM.

This structure may evolve as KAI grows.

---

## 16. Implementation Technologies

Initial toolchain:

- C++23
- LLVM
- CMake
- Ninja
- Git
- GitHub Actions

Testing framework has not yet been selected.

---

## 17. Architectural Rules

1. Lexer must not perform parsing.
2. Parser must not perform code generation.
3. Semantic analysis must be independent from LLVM.
4. AST must not contain LLVM objects.
5. Diagnostics must be structured.
6. Source spans must survive through compiler stages where useful.
7. LLVM must remain a backend implementation detail.
8. Compiler architecture must not depend on AI tooling.
9. AI tooling should consume compiler semantic information.
10. Optimization claims must be benchmarked and reproducible.

---

## 18. Self-Hosting

KAI-CC will initially be written in C++23.

A future self-hosted compiler may follow this bootstrap process:

    Stage 0
    KAI compiler written in C++

        |
        v

    Stage 1
    KAI compiler written in KAI,
    compiled by Stage 0

        |
        v

    Stage 2
    Stage 1 compiles itself

Self-hosting is not part of KAI 0.1.