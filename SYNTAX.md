# KAI Syntax

> Status: Experimental Draft  
> Target version: KAI 0.1

This document describes the proposed syntax for the first implementation of KAI.

Nothing in this document should be considered stable until KAI reaches a versioned language specification.

---

## 1. File Extension

KAI source files use:

    .kai

Example:

    main.kai

---

## 2. Hello World

    fn main() {
        print("Hello from KAI")
    }

Semicolons are not required.

---

## 3. Comments

Single-line comments:

    // This is a comment

Block comments are not required for KAI 0.1.

---

## 4. Variables

Immutable variables are declared using `let`.

    let age = 20

Explicit types are optional when they can be inferred.

    let age: i32 = 20

Mutable variables use `mut`.

    mut counter = 0

    counter = counter + 1

Reassignment of immutable values is a compile-time error.

---

## 5. Primitive Types

Initial integer types:

    i8
    i16
    i32
    i64

    u8
    u16
    u32
    u64

Floating-point types:

    f32
    f64

Additional basic types:

    bool
    char
    str

Examples:

    let age: i32 = 21
    let temperature: f32 = 24.5
    let active: bool = true
    let name: str = "KAI"

---

## 6. Functions

Functions use `fn`.

    fn add(a: i32, b: i32) -> i32 {
        return a + b
    }

Functions without a return value omit the return type.

    fn greet(name: str) {
        print(name)
    }

KAI may eventually support implicit final-expression returns, but KAI 0.1 will initially prefer explicit `return`.

---

## 7. Main Function

Program execution begins at:

    fn main() {
        ...
    }

An integer-returning main function may also be supported:

    fn main() -> i32 {
        return 0
    }

---

## 8. Operators

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

Reference:

    &
    &mut

See GRAMMAR.md §14 (reference types) and §35 (unary expressions).

Range:

    ..

See GRAMMAR.md §32. The upper bound is exclusive.

Assignment:

    =

Compound assignment operators may be added later.

---

## 9. Conditional Statements

    if age >= 18 {
        print("adult")
    } else {
        print("minor")
    }

Parentheses around conditions are not required.

---

## 10. While Loops

    mut i = 0

    while i < 10 {
        print(i)
        i = i + 1
    }

---

## 11. For Loops

Syntax:

    for i in 0..10 {
        print(i)
    }

**KAI LANGUAGE M6 (post-alpha.2): implemented and executable** for exactly this form - a `for` loop whose
iterable is a literal `start..end` integer range. `i` is scoped to the loop body only, is always immutable
(reassigning it is a compile-time error, same diagnostic as reassigning any other `let`), and receives
successive values of the range's own element type (both endpoints must resolve to the same concrete integer
type, via the same contextual-literal-adaptation rules arithmetic already uses - e.g. `for i in 0..n` with
`n: u32` adapts the literal `0` to `u32`). `start`/`end` are each evaluated exactly once, before the loop
begins - never re-evaluated per iteration.

Range semantics:

    0..10

means the half-open range:

    0 <= i < 10

so `start >= end` executes the loop body zero times.

**Not yet implemented:** iteration over anything other than a literal integer range (arrays, general
iterators, a first-class `Range` value), an inclusive `..=` range, reverse iteration, a `step`, and
`break`/`continue`. `let r = 0..10` (a range used as an ordinary value, outside a `for` loop) also remains
unsupported.

---

## 12. Arrays

Fixed-size array literal:

    let values = [1, 2, 3, 4]

Explicit type - `[T; N]`, NOT the slice syntax `[T]` (§16 below covers
slices, a distinct, still-future, non-owning view type):

    let values: [i32; 4] = [1, 2, 3, 4]

Indexing:

    let first = values[0]

Mutable element write (requires a `mut` binding):

    mut values = [1, 2, 3, 4]
    values[0] = 5

**KAI LANGUAGE M7A (post-alpha.2): the type system is implemented** for
exactly this - `[T; N]` is a real semantic type (element type and
compile-time length `N` are both part of the type's own identity, so
`[i32; 3]` and `[i32; 4]` are distinct types), and both forms above
(inferred and explicitly-annotated) resolve and type-check correctly,
rejecting a non-homogeneous literal (`[1, true, 3]`).

**KAI LANGUAGE M7B (post-alpha.2): native execution is implemented** for
a LOCAL fixed-size array - literal creation, checked indexed reads,
checked indexed writes through a `mut` binding, and integration with an
M6 `for i in start..end` loop all compile to a real, running native
executable. Indexing is CHECKED (TYPE_SYSTEM.md §18's approved design):
a compile-time-constant out-of-bounds index (`values[4]`/`values[-1]`
for a length-4 array) is rejected at compile time; a dynamic
out-of-bounds index traps the program immediately at runtime (not the
language `panic` mechanism - no unwinding, no recovery, no stable exit-
code guarantee) rather than ever reading or writing out of bounds.
Still NOT implemented: arrays as a function parameter or return type (no
array calling convention/ABI exists), whole-array assignment/copy (`let
b = a` / `a = b` for two array-typed values), and slice syntax (`[T]`,
§16 below - still fully deferred, not a semantic type at all yet).

---

## 13. Strings

    let name = "KAI"

String interpolation is not required for KAI 0.1.

Initial output:

    print("Hello")
    print(name)

A string literal's type is `str` - see TYPE_SYSTEM.md §13-17 for the full
str/String design.

Current implementation status:

    string literals              implemented
    inferred str locals          implemented
    print(str)                   implemented
    explicit `: str` annotation  implemented
    str parameters/returns       implemented
    String                       not implemented
    provenance/borrow checking   not implemented

`let name = "KAI"` and `print(...)` shown above, and the explicit `str`
annotations/parameters/returns shown elsewhere in this document (§5, §6),
all already work end-to-end in the reference compiler. `String` is not
implemented.

---

## 14. Type Inference

KAI should infer types when the result is unambiguous.

    let x = 42

Equivalent to:

    let x: i32 = 42

Default literal types for KAI 0.1:

    integer literal -> i32
    floating literal -> f64
    true / false -> bool
    string literal -> str

These defaults may change before the language becomes stable.

---

## 15. Type Conversions

Implicit type conversions should be limited.

This should be rejected:

    let age: i32 = "20"

Explicit conversion syntax is not yet finalized.

Possible future direction:

    let value = i64(x)

---

## 16. Structs

Structs are planned shortly after the core KAI 0.1 compiler is operational.

Proposed syntax:

    struct User {
        id: u64
        name: str
        active: bool
    }

Construction syntax is still under design.

Possible direction:

    let user = User {
        id: 1,
        name: "Jaret",
        active: true
    }

Struct literal fields are comma-separated (see GRAMMAR.md §44).

---

## 17. Modules

Module syntax is not required for the first compiler milestone.

Proposed future direction:

    use math
    use net.http

Exact module semantics will be designed separately.

---

## 18. Generics

Generics are not part of the initial KAI 0.1 implementation.

Future direction:

    fn first<T>(values: [T]) -> T {
        return values[0]
    }

---

## 19. Traits

Traits are not part of KAI 0.1.

Possible future syntax:

    trait Display {
        fn display(self) -> str
    }

    impl Display for User {
        fn display(self) -> str {
            return self.name
        }
    }

---

## 20. Unsafe Operations

Low-level memory operations may eventually require explicit unsafe blocks.

Possible direction:

    unsafe {
        // low-level operation
    }

No unsafe operations are required for the first KAI implementation.

---

# KAI 0.1 Minimum Syntax

The first compiler only needs to understand:

- literals
- variables
- primitive types
- arithmetic
- comparisons
- functions
- function calls
- return
- if / else
- while
- basic for loops
- basic arrays
- print

Everything else should wait until this subset is stable.