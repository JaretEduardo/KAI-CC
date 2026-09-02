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

### Current Implementation Status (as of LLVM Codegen Milestone 7)

The ACTUAL current pipeline (see `compiler/include/kai/codegen/LLVMCodeGenerator.hpp`'s own class comment) is:

    Source -> Lexer -> Parser/AST -> SemanticAnalyzer -> TypeChecker -> ControlFlowAnalyzer ->
    LLVMCodeGenerator -> LLVM IR -> native-entry adaptation -> LLVMObjectEmitter -> native Object File ->
    NativeLinker -> Native Binary

This skips the HIR stage shown in the diagram above: `LLVMCodeGenerator` lowers AST + SemanticModel directly to
LLVM IR for the MVP. This is a deliberate, deadline-driven decision, not an abandonment of HIR - HIR remains
planned (see §8) once the frontend/backend MVP is stable, consistent with this section's own "introduced after
the frontend is stable" framing.

Currently implemented and native-executable-verified on Fedora Linux x86_64 only (no cross-compilation, no
Windows/macOS support yet): primitive integer/Bool/f32/f64 values and arithmetic/comparison/logical
expressions (`&&`/`||` are FINAL short-circuit, not eager), local variables, mutability/assignment, function
parameters/calls/forward-calls/recursion, `if`/`else`/`else if`, `while`, `for i in start..end` over an
integer range (KAI LANGUAGE M6, post-alpha.2), a fixed-size array `[T; N]` (including nested fixed arrays)
with general checked indexed reads/writes at any nesting depth (KAI LANGUAGE M7B/M9, post-alpha.2 - a
compile-time-constant out-of-bounds index is a compile error; a dynamic one traps via `llvm.trap`, never an
unchecked access, independently checked at EACH level of a nested index), whole-array initialization/
assignment/self-assignment and array function parameters/returns lowered as a direct LLVM aggregate `[N x T]`
argument/result (KAI LANGUAGE M8B, post-alpha.2 - no `sret`/`byval`/hidden pointer, no promised stable
external C ABI), Unit functions, a minimal `print` builtin for those primitive types (lowered to the tiny
static C runtime described in §12, `runtime/kai_runtime.c`), native object emission (`LLVMObjectEmitter`),
and static runtime linking into a real executable (`NativeLinker`, invoking the host C compiler driver) via
`kaicc <file.kai> -o <output>`.

Still unimplemented (backend/native execution only): general iterable/foreach semantics and array/slice
iteration (`for x in someArray` - `for` only supports a literal `start..end` integer range, KAI LANGUAGE M6),
a first-class `Range` value, slices (`[T]` is now a real semantic `TypeKind::Slice` as of KAI LANGUAGE M10A,
post-alpha.2 - see this section's own M10A note below - but has NO LLVM lowering, no array-to-slice
conversion, and no slice indexing yet), `Char` as a backend-lowerable value, references, ownership/borrowing,
structs/enums/generics, `panic`/`assert` lowering, optimization passes, HIR, an LSP, and the higher-level
`kai` CLI wrapper described in §14 (only `kaicc` itself exists today).

**WINDOWS M1 (portability baseline, in progress):** the same source tree also builds and runs its CTest suite
natively on Windows x86_64, targeting exactly one Windows toolchain baseline - MSYS2 UCRT64 with Clang/LLVM
22.1.8 (never MSVC, never WSL) - via a dedicated `compiler-windows` CI job. This is a build/test portability
milestone only, not a packaged Windows release: no Windows tag/release/version bump has happened, and
README.md's platform claim is not updated until a packaging milestone actually validates end-to-end Windows
distribution. The only native-code changes this required were `NativeLinker::currentExecutablePath()` (Windows
has no `/proc/self/exe`; uses `GetModuleFileNameW` instead) and `kai::cli::resolveNativeExecutablePath()` (a
small, Windows-only `.exe`-suffix decision for `-o <output>`, since MinGW-style host compiler drivers' own
output-naming behavior around a missing extension is not something to depend on implicitly) - everything else
in the pipeline (CMake's own `WIN32`/executable-suffix handling, LLVM's host-triple object emission, the
`cc`/`clang`/`gcc` driver search, the `libkai_runtime.a` static-archive naming) already worked unmodified,
since this baseline deliberately avoids MSVC's differing conventions (`.lib`, `cl.exe`) entirely.

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

Not yet implemented: unreachable-code analysis, constant-condition flow reasoning (e.g. recognizing `if true` or `while true` as always executing), divergence analysis, a general control-flow graph, for-loop iterable/element type validation beyond a literal `start..end` integer range (KAI LANGUAGE M6, post-alpha.2, implements exactly that one form - general iterable/foreach semantics, array/slice iteration, and a first-class `Range` value remain unimplemented), member assignment semantics (`obj.field = value` - structs don't exist yet; `xs[index] = value` for a mutable LOCAL array IS implemented, KAI LANGUAGE M7B, post-alpha.2 - see TYPE_SYSTEM.md §18), builtin call signature validation, reference semantic types, ownership/borrow checking, and first-class function/method call semantics. Semantic analysis as a whole is not yet complete.

**Compound semantic types (KAI LANGUAGE M7A, post-alpha.2):** `semantic::Type` (`compiler/include/kai/semantic/Type.hpp`) started as a flat, closed `TypeKind` enum wrapper with no structural payload - sufficient while every modeled type was fully self-describing from its own kind tag alone. Fixed-size arrays (`[T; N]`) broke that assumption: an array's identity also depends on an element type and a compile-time length, neither of which fits in a flat enum. Rather than turning every `Type` into a heap object/shared_ptr (which would make even `i32` expensively allocated), `Type` instead carries a small, cheap, trivially-copyable `CompoundTypeId` handle alongside its `TypeKind`, meaningful only for compound kinds (currently only Array) and ignored entirely by every primitive kind. The actual structural data (`ArrayTypeInfo { elementType, length }`) lives in ONE compile-scoped interning table OWNED BY `SemanticModel` itself - not a process-global singleton, not a second parameter every pass has to thread through separately, since every pass (SemanticAnalyzer, TypeChecker, LLVMCodeGenerator, and every semantic-tooling consumer) already receives the same `SemanticModel` by reference for the whole compilation's lifetime. `SemanticModel::internArray()` canonicalizes structurally-equal array shapes to the SAME `CompoundTypeId`, so `[i32; 3] == [i32; 3]` compares true by simple value equality, with no deep structural comparison needed at the `Type` level itself. A `Type` carrying a `CompoundTypeId` is only ever valid against the SAME `SemanticModel` that produced it - the same lifetime discipline `SymbolId` already established for symbol identity. Semantic tooling (`SemanticTypeName::typeName()`) reads this same interning table (via `SemanticModel::arrayElementType()`/`arrayLength()`, never a raw `CompoundTypeId`) to render `[i32; 3]` canonically, without any internal ID ever reaching JSON output. This design generalizes to a future second compound `TypeKind` (e.g. a struct/tuple) without further redesign - M7A itself adds exactly one (Array), and KAI LANGUAGE M10A (post-alpha.2) is exactly that predicted generalization arriving: a second compound kind, Slice, added via its own sibling interning table with no change to `CompoundTypeId`/`Type` themselves (see this section's own M10A note below).

**Semantic value passing vs. physical ABI lowering (KAI LANGUAGE M8A, post-alpha.2):** M8A is a deliberate
architectural split, not merely a scheduling one. The FRONTEND (SemanticAnalyzer/TypeChecker) resolves and
enforces KAI's language-level value semantics for fixed-size arrays - value-copy assignment/initialization
with no aliasing, exact structural type matching, by-value function parameters/returns - entirely through
the SAME generic, Type-kind-agnostic machinery every other Type already used (Type equality, contextual
literal adaptation, TypeMismatch, argument/return-type checking); this required ZERO new TypeChecker.cpp
code, since none of that machinery ever special-cased "is this an aggregate." The BACKEND (LLVMCodeGenerator)
is a completely separate concern: it decides HOW an array value physically crosses a function boundary or
gets copied in memory (LLVM aggregate arguments/results, registers, stack, hidden temporaries, indirect/
`sret`/`byval`-style lowering, ...), and none of those choices may ever change the frontend's already-
settled observable semantics. M8A itself intentionally implemented none of that backend lowering - the M7B
guards (`declareFunction()`'s explicit Array parameter/return rejection, `lowerAssignmentExpr()`'s/
`generateArrayVarDeclStmt()`'s explicit whole-array-copy rejection) stayed in place unchanged through M8A, so
a semantically-valid array-parameter/return or whole-array-copy program still failed cleanly at code
generation. **KAI LANGUAGE M8B (post-alpha.2)** then removed exactly those guards and implemented the chosen
backend strategy: DIRECT LLVM aggregate arguments/results - `[T; N]` lowers straight to `[N x T]` as a
`FunctionType` parameter/return type, with no `sret`, no `byval`, no hidden pointer parameter, and no
reference/aliasing introduced anywhere in the lowering. `ArrayLiteralExpr` became a genuine value-producing
expression for the first time (a temporary entry-block stack slot, initialized element-by-element, then
loaded once as a complete aggregate value) so it can be used as a call argument or return expression, not
only as a `VarDecl`'s own direct initializer. KAI still does not promise a stable, external, C-compatible ABI
for how arrays cross a function boundary - the direct-aggregate choice remains an internal backend
implementation detail, never a language-design guarantee (see DESIGN_QUESTIONS.md's own M8A/M8B resolution).

**Nested fixed-array indexing (KAI LANGUAGE M9, post-alpha.2):** before M9, `matrix[0][1]` failed at code
generation even though the FRONTEND already type-checked it correctly - `TypeChecker::checkIndexExpr()`'s own
recursive `object`-type inference already handled arbitrary nesting depth with zero changes needed (the
nested-array TYPE rule from M7A was never the gap). The gap was entirely in `LLVMCodeGenerator`:
`lowerArrayElementAddress()` only accepted a direct (through transparent `ParenExpr` only) `IdentifierExpr` as
an `IndexExpr`'s object, so the OUTER `IndexExpr` in `matrix[0][1]` (whose own object, `matrix[0]`, is itself
an `IndexExpr`, not an identifier) was rejected outright - a real, narrow, codegen-only limitation, not a
frontend one. M9 generalizes this via a new `lowerArrayBase()` helper that resolves an `IndexExpr`'s object to
an address either way: a direct Local/Parameter identifier (M7B/M8B's own case, unchanged), or - new in M9 -
another `IndexExpr`, resolved by recursing into `lowerArrayElementAddress()` itself, which fully computes
THAT level's own bounds check and GEP before returning, before the caller ever treats the result as its own
array storage. This makes `matrix[i][j]` (and deeper, `a[i][j][k]`) work by the SAME per-level function
calling itself once per nesting level, each level's bounds check/GEP strictly ordered after the previous
level's has already succeeded - no source-level reference/lvalue system was introduced to make this work. The
one FRONTEND change M9 needed was narrower still: `checkIndexAssignmentTarget()`'s root-identifier lookup
(previously a single-level-only unwrap) was generalized to
`unwrapIndexAssignmentRootIdentifier()`, which walks through zero or more nested `IndexExpr` layers to the
SAME kind of root a single-level `xs[i] = v` already required - a bare identifier resolving to a
`SymbolKind::Local` binding - so mutability through `matrix[i][j] = v` is still decided by the ROOT binding
alone, exactly as the language always intended (TYPE_SYSTEM.md §18's own "Nested Fixed-Array Indexing"
subsection), never by an intermediate array element.

**Slice TYPE foundation (KAI LANGUAGE M10A, post-alpha.2):** `TypeKind::Slice` gives `[T]` a real semantic
Type - structurally distinct from Array (`TypeKind::Array`): a Slice's identity is its element Type ALONE,
with NO length component, since a slice's length is runtime data rather than part of the type
(TYPE_SYSTEM.md's own "Slices" section). This reuses M7A's own compound-type-context architecture (see this
section's own M7A note above) via a SEPARATE sibling interning table (`SemanticModel`'s `sliceTypes_`,
alongside Array's own `arrayTypes_`) rather than a unified tagged compound-type table: Array and Slice have
different structural shapes (length vs. no length), so a shared table would need a variant/tagged payload for
exactly two current cases, adding indirection with no present benefit - two small sibling tables is the
least-invasive extension of the existing design, and generalizes to a future third compound kind (a tuple, a
reference, a function type, ...) by adding another sibling table, not by redesigning `CompoundTypeId`/`Type`
itself. `SemanticAnalyzer::resolveSliceTypeSyntax()` mirrors `resolveArrayTypeSyntax()`'s own recursive
Error/Unresolved-propagation discipline exactly, with no length to decode. Backend support remains entirely
absent by design: `LLVMCodeGenerator::lowerType()` returns the same `nullptr` signal every other unsupported
Type already returns for Slice (grouped with Unresolved/Error/Char) - a semantically well-typed slice
parameter/return still fails CLEANLY at code generation (the existing `unsupportedConstruct()` diagnostic
path, unchanged), never a crash, never silently treated as Array or `str`. The eventual runtime representation
is expected to be an LLVM `{ptr, i64}`-shaped struct (distinct from Array's own direct `[N x T]` aggregate -
see this section's own M8A/M8B note above), but M10A implements none of that lowering. Lifetime/source-
validity concerns (a slice must not outlive the storage it views) are a SEMANTIC question a future dedicated,
deliberately limited lifetime checker must answer - they are not solved, or even touched, by choosing an LLVM
pointer representation, and KAI still has no general borrow checker of any kind. See TYPE_SYSTEM.md's own
"Slices" section and DESIGN_QUESTIONS.md's own M10A resolution/open-questions.

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

Testing framework: no external framework - a tiny in-repo CHECK macro (`tests/support/check.hpp`), registered
as CTest suites (see the `kai_add_test(...)` CMake helper under `tests/`).

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