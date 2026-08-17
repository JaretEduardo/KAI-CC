# KAI 0.1 Scope

> Status: Draft Freeze Candidate
> Purpose: define the implementation boundary for the first usable KAI compiler.

## 1. Goal

KAI 0.1 exists to prove the core language and compiler architecture.

The goal is NOT to create a production-ready ecosystem.

The primary milestone is:

    kai run hello.kai

producing a native executable.

---

# 2. Required Compiler Pipeline

KAI 0.1 must implement:

    Source
      ↓
    Lexer
      ↓
    Tokens
      ↓
    Parser
      ↓
    AST
      ↓
    Semantic Analysis
      ↓
    HIR
      ↓
    LLVM IR
      ↓
    Native Object
      ↓
    Executable

KAI 0.1 does not require MIR or KAI IR.

---

# 3. Required Primitive Types

Initial required types:

    i32
    i64
    u32
    u64

    f32
    f64

    bool
    char

    ()

Additional integer widths may be implemented when convenient.

---

# 4. Required Variables

KAI 0.1 supports:

    let x = 10

    let x: i64 = 10

    mut x = 10

    mut x: i64 = 10

Immutable variables cannot be reassigned.

Mutable variables can be reassigned when types remain compatible.

---

# 5. Required Type Inference

Local inference:

    let x = 42

must work.

Function parameter inference is not required.

Function signatures remain explicit.

---

# 6. Required Functions

KAI 0.1 supports:

    fn add(a: i32, b: i32) -> i32 {
        return a + b
    }

and:

    fn greet() {
        print("Hello")
    }

Required capabilities:

- parameters
- return values
- function calls
- recursion
- local variables
- nested scopes

---

# 7. Required Operators

Arithmetic:

    +
    -
    *
    /
    %

Comparison:

    ==
    !=
    <
    <=
    >
    >=

Logical:

    &&
    ||
    !

Assignment:

    =

---

# 8. Required Control Flow

KAI 0.1 supports:

    if
    else if
    else

    while

    for ... in 0..n

    return

---

# 9. Required Strings

The first milestone only needs string literals sufficient for:

    print("Hello from KAI")

Complete dynamic `String` functionality may be implemented later during KAI 0.1 development.

---

# 10. Required Arrays

Basic fixed array literals should eventually work:

    let values = [1, 2, 3]

Basic indexing:

    values[0]

Full Buffer and slice support may follow after the first native executable.

---

# 11. Required Diagnostics

Compiler errors must contain:

- error code
- human-readable message
- file
- line
- column
- source span where possible

Example:

    error[E0301]: incompatible types

     --> src/main.kai:4:20

    expected:
        i32

    found:
        &str

JSON diagnostics are planned but do not block the first native executable.

---

# 12. Required Semantic Checks

Initial semantic analyzer should eventually detect:

- undefined symbols
- duplicate local symbols
- incompatible types
- invalid assignments
- assignment to immutable bindings
- invalid function argument count
- invalid function argument types
- missing return values
- invalid condition types

---

# 13. Ownership Scope

The complete ownership model is specified separately.

The first compiler milestone does NOT need the entire borrow checker.

Implementation order:

1. Copy primitive values
2. Move resource values
3. basic use-after-move checking
4. immutable borrow
5. mutable borrow
6. borrow conflicts

Ownership functionality should be introduced incrementally.

---

# 14. Error Handling Scope

The complete error model includes:

    Result<T, E>
    Ok(...)
    Err(...)
    ?

These features do not block the first Hello World milestone.

They should be implemented after basic functions and native code generation work.

---

# 15. Struct Scope

Structs are part of KAI 0.1 language development but do not block the first compiler milestone.

Required later:

    struct Point {
        x: f32
        y: f32
    }

    let point = Point {
        x: 1.0,
        y: 2.0
    }

---

# 16. Module Scope

Initial compiler may begin with one source file.

Then add:

    use

and file-based module discovery.

Package dependencies are not required initially.

---

# 17. CLI Scope

Initial low-level compiler:

    kaicc input.kai -o output

Then introduce:

    kai build
    kai run
    kai check

The first CLI does not need package management.

---

# 18. Initial Runtime Scope

Initial runtime only needs functionality required for:

    print

and basic panic support.

The runtime must remain small.

---

# 19. Not Required for KAI 0.1 First Milestone

Do NOT block initial development on:

- generics
- traits
- pattern matching
- package registry
- HTTP
- networking
- async/await
- threads
- GPU support
- SIMD
- macros
- metaprogramming
- closures
- garbage collection
- custom allocators
- raw pointers
- FFI
- full debugger
- LSP
- formatter
- self-hosting

---

# 20. First Compiler Milestone

Input:

    fn main() {
        print("Hello from KAI")
    }

Command:

    kaicc hello.kai -o hello

Execution:

    ./hello

Output:

    Hello from KAI

This milestone proves the complete compiler pipeline.

---

# 21. Second Compiler Milestone

Input:

    fn add(a: i32, b: i32) -> i32 {
        return a + b
    }

    fn main() {
        let result = add(20, 22)
        print(result)
    }

Output:

    42

This proves:

- local variables
- integer types
- functions
- arguments
- return values
- native arithmetic

---

# 22. Third Compiler Milestone

Input:

    fn fibonacci(n: i32) -> i32 {
        if n <= 1 {
            return n
        }

        return fibonacci(n - 1) + fibonacci(n - 2)
    }

    fn main() {
        for i in 0..10 {
            print(fibonacci(i))
        }
    }

This proves:

- recursion
- comparisons
- if
- ranges
- loops
- nested calls

---

# 23. Fourth Compiler Milestone

Introduce:

- structs
- arrays
- ownership
- borrowing
- Result

At this point KAI begins exercising its unique language semantics rather than only basic compiler functionality.

---

# 24. AI-Native Milestone

After the semantic frontend is stable, introduce:

    kai inspect
    kai refs
    kai type

Then:

    kai deps
    kai impact

These commands should consume information already produced by the compiler semantic model.

AI tooling must not be embedded into the language syntax itself.

---

# 25. Optimization Milestone

Only after KAI programs compile reliably should KAI introduce:

    MIR
    KAI IR

and compiler-owned optimization passes.

Potential first passes:

- constant folding
- constant propagation
- dead code elimination
- CFG simplification

Performance claims must always be benchmarked.

---

# 26. Definition of Done for KAI 0.1

KAI 0.1 can be considered successful when:

- KAI source compiles to native code
- variables work
- functions work
- control flow works
- basic static typing works
- useful compiler diagnostics exist
- basic structs work
- initial ownership rules work
- Result-based errors work
- multi-file modules work
- `kai build`
- `kai run`
- `kai check`

The language does not need to be production-ready.

---

# 27. Freeze Rule

Once compiler implementation begins:

- syntax changes require a documented reason
- new features should not be added casually
- examples must remain valid or be intentionally migrated
- specification and implementation should evolve together

KAI 0.1 is allowed to change.

It should not change randomly.

---

# 28. Core Scope Rule

If a feature is not required to prove KAI's core language, compiler, memory model, or AI-native semantic tooling, it should probably wait until after KAI 0.1.