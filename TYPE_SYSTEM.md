# KAI Type System

> Status: Experimental Draft
> Target version: KAI 0.1+
> Last updated: August 2026

## 1. Overview

KAI is a statically typed programming language.

Every expression has a type known by the compiler before native code generation.

The type system is designed around the following goals:

* predictable semantics
* strong compile-time validation
* minimal implicit behavior
* concise local type inference
* explicit public APIs
* compatibility with KAI's ownership model
* efficient native representation
* clear information for humans and AI coding agents

KAI favors simple and explicit type rules over complex automatic conversions.

---

# 2. Static Typing

KAI determines the type of every value during compilation.

Example:

```kai
let age = 21
```

The compiler infers:

```text
age: i32
```

Invalid operations should be rejected before code generation.

Example:

```kai
let age: i32 = "twenty"
```

must produce a compile-time error.

---

# 3. Primitive Types

KAI initially provides the following primitive types.

## Signed Integers

```text
i8
i16
i32
i64
```

Sizes:

```text
i8     8 bits
i16   16 bits
i32   32 bits
i64   64 bits
```

---

## Unsigned Integers

```text
u8
u16
u32
u64
```

Sizes:

```text
u8     8 bits
u16   16 bits
u32   32 bits
u64   64 bits
```

---

## Floating Point

```text
f32
f64
```

KAI floating-point values follow IEEE 754 semantics where supported by the target architecture.

---

## Boolean

```text
bool
```

Possible values:

```kai
true
false
```

KAI does not provide implicit truthiness.

Invalid:

```kai
let value = 10

if value {
    ...
}
```

Valid:

```kai
if value != 0 {
    ...
}
```

Conditions must evaluate to `bool`.

---

## Character

```text
char
```

A `char` represents one Unicode scalar value.

Example:

```kai
let letter: char = 'K'
```

A `char` is not equivalent to `u8`.

UTF-8 encoded text may require multiple bytes for one character.

---

# 4. Primitive Copy Semantics

Primitive scalar types use `Copy` semantics.

Initial Copy types:

```text
i8
i16
i32
i64

u8
u16
u32
u64

f32
f64

bool
char
str
```

`str` is included here even though it is not a primitive scalar: it is a
small, fixed-size, non-owning text view (see §15) rather than an owned
resource, so copying it is exactly as cheap and ownership-free as copying an
integer. `String` (§14) is the future owned counterpart and is a `Move` type,
not `Copy` - see §5 in MEMORY_MODEL.md for the general Copy/Move value
categories this fits into.

Example:

```kai
let a = 42
let b = a

print(a)
print(b)
```

Both values remain valid.

Copying these values does not transfer ownership.

---

# 5. Integer Literals

Integer literals without additional type information default to:

```text
i32
```

Example:

```kai
let value = 42
```

is inferred as:

```text
i32
```

However, literals may use contextual typing.

Example:

```kai
let value: i64 = 42
```

This does not require an explicit conversion because the literal itself can be represented directly as `i64`.

This is different from converting an existing `i32`.

Example:

```kai
let first: i32 = 42
let second: i64 = first
```

must be rejected.

The programmer must write an explicit conversion:

```kai
let second = i64(first)
```

---

# 6. Floating-Point Literals

Floating-point literals default to:

```text
f64
```

Example:

```kai
let temperature = 24.5
```

is:

```text
f64
```

Context may determine another type:

```kai
let temperature: f32 = 24.5
```

Literal suffix syntax may be introduced later if real programs demonstrate a need for it.

Possible future syntax:

```kai
let size = 100u64
let ratio = 0.5f32
```

This syntax is not required for the earliest KAI compiler milestone.

An integer literal and a floating-point literal are separate families, and
contextual typing never crosses that boundary:

```kai
let x: i64 = 10
```

is valid (an integer literal adopting a contextual integer type), but:

```kai
let x: f64 = 10
```

is a type mismatch - an integer literal never adopts a floating-point
contextual type. The literal itself must be written as a float:

```kai
let x: f64 = 10.0
```

The same rule applies symmetrically: a floating-point literal never adopts a
contextual integer type. This is not treated as an implicit conversion in
either direction - it is a rule about which contextual types an integer or
floating-point literal may resolve to in the first place.

---

# 7. Numeric Conversions

KAI does not silently convert existing numeric values between different types.

Invalid:

```kai
let small: i32 = 42
let large: i64 = small
```

Valid:

```kai
let large = i64(small)
```

The same rule applies to:

```text
signed -> unsigned
unsigned -> signed
integer -> float
float -> integer
f32 -> f64
f64 -> f32
```

Conversions should remain explicit.

This rule is intended to avoid hidden:

* truncation
* precision loss
* signedness changes
* overflow behavior

---

# 8. Integer Overflow

KAI should provide deterministic integer overflow behavior.

Compile-time constant overflow must be an error.

Example:

```kai
let value: u8 = 300
```

must fail during compilation.

For ordinary safe runtime arithmetic, the initial language direction is:

> Integer overflow causes a deterministic runtime failure rather than silently wrapping.

KAI should avoid different arithmetic semantics between development and optimized builds.

Explicit wrapping operations may be introduced later.

Possible future APIs:

```kai
a.wrapping_add(b)
a.saturating_add(b)
a.checked_add(b)
```

The exact APIs are not part of KAI 0.1.

---

# 9. Division

Integer division uses integer arithmetic.

Example:

```kai
let value = 10 / 3
```

with two `i32` operands produces:

```text
3
```

Floating-point division requires floating-point operands.

```kai
let value = 10.0 / 3.0
```

Division by zero for integer values must produce a deterministic runtime failure.

Floating-point division follows IEEE 754 behavior.

---

# 10. Local Type Inference

KAI supports type inference for local variables.

Example:

```kai
let age = 21
let active = true
let ratio = 0.5
```

The compiler determines:

```text
age:    i32
active: bool
ratio:  f64
```

Explicit types remain available:

```kai
let age: u64 = 21
```

Type inference should reduce boilerplate without hiding important API information.

---

# 11. Function Signatures

Function parameter types must be explicit.

Valid:

```kai
fn add(a: i32, b: i32) -> i32 {
    return a + b
}
```

Invalid:

```kai
fn add(a, b) {
    return a + b
}
```

Functions returning a value must also declare their return type.

Example:

```kai
fn get_age() -> i32 {
    return 21
}
```

Functions that do not declare a return type implicitly return the unit type.

Example:

```kai
fn greet(name: str) {
    print(name)
}
```

is conceptually equivalent to:

```kai
fn greet(name: str) -> () {
    print(name)
}
```

Explicit `-> ()` is normally unnecessary.

---

# 12. Unit Type

KAI provides the unit type:

```text
()
```

The unit type represents successful computation without a meaningful value.

Example:

```kai
fn print_message() {
    print("Hello")
}
```

conceptually returns:

```text
()
```

The unit type is particularly useful with `Result`.

Example:

```kai
fn save() -> Result<(), IOError> {
    ...

    return Ok(())
}
```

---

# 13. String Model

KAI distinguishes between an owned, growable text buffer and a non-owning
view over text:

```text
str       Copy, immutable, non-owning UTF-8 text view
String    Move, owned, growable UTF-8 buffer (future - see §14)
```

`str` is a first-class, concrete, sized value type - not a reference, and not
an unsized type the way some other languages' `str` is. It is the ordinary
way to spell "a function reads text" (§15). Earlier drafts of this document
described `str` as unable to exist as a standalone value, reachable only
through `&str`; that description has been superseded by the definition in
§15 below.

**Current implementation status:**

```text
string literals              implemented
inferred str locals          implemented
print(str)                   implemented
explicit `: str` annotation  implemented
str parameters/returns       implemented
String                       not implemented
provenance/borrow checking   not implemented
```

`str` is a spellable source-level type annotation - `let x: str = "hello"`,
`fn f(x: str)`, and `fn f() -> str` all compile end-to-end in the reference
compiler (Spellable str + Parameters/Returns MVP). A temporary, narrow
return-safety restriction applies to functions with more than one `str`
parameter (see §15). `String` does not exist in the compiler at all yet, and
no general return-provenance/borrow checker exists.

---

# 14. String (future)

`String` represents owned, growable UTF-8 text whose storage is controlled by
the value. `String` is **not implemented** by the current compiler - this
section describes the intended future design.

Example:

```kai
let name = String("KAI")
```

`String` uses Move semantics.

Example:

```kai
let first = String("KAI")
let second = first
```

Ownership transfers to `second`.

Using `first` afterward is invalid.

```kai
print(first)
```

must produce a use-after-move diagnostic.

An independent copy requires:

```kai
let second = first.clone()
```

---

# 15. str

`str` is the ordinary text-view type: an immutable, `Copy`, non-owning view
over UTF-8 text, conceptually `{ ptr, len }`. Passing a `str` copies this
small, fixed-size descriptor - never the text itself - and never allocates.

Ordinary APIs read text with bare `str`, not a reference:

```kai
fn greet(name: str) {
    print(name)
}
```

This means:

* `greet` does not own the text `name` views
* `greet` cannot modify the text
* ownership of the underlying bytes remains wherever it already was (static
  program storage for a literal; eventually a `String` or another owner)

A future `&str` (a reference to a `str` descriptor) is unnecessary for
ordinary text borrowing, since `str` already is the non-owning, `Copy` view -
it is not part of KAI 0.1. Whether a general reference to a `str` value is
ever useful once KAI has a general reference/borrow design remains an open
question (DESIGN_QUESTIONS.md).

Owned, growable text uses `String` (§14, future).

`str` is a valid function return type (`fn language() -> str { return "KAI" }`,
`fn identity(value: str) -> str { return value }`). A function with more than
one `str` parameter may only return a `str` that is itself a literal - a
temporary, narrow implementation-boundary restriction (not a general
provenance rule) documented in full in MEMORY_MODEL.md §25.

---

# 16. String Literals

A string literal:

```kai
"Hello"
```

represents immutable program data, and has the concrete type `str`.

Example:

```kai
let message = "Hello"
```

The compiler stores string literals in read-only static program memory. A
`str` obtained from a literal is valid for the entire program's lifetime -
it never needs a validity check, since static program storage is never
destroyed. This already holds in the current implementation, not only in the
proposed design: no heap allocation is required merely to use a string
literal today.

---

# 17. Coercing String to str (future)

(Depends on `String`, §14 - not yet implemented.)

An owned `String` is intended to be viewable as `str` for reading, as an
implicit, non-allocating, ownership-preserving coercion:

```kai
let name = String("KAI")

greet(name)
```

where:

```kai
fn greet(name: str) {
    print(name)
}
```

The exact coercion mechanism will be finalized alongside the standard
library. Such a conversion must not transfer ownership or allocate memory,
and a `str` obtained this way remains valid only as long as the `String` it
was borrowed from is not moved or mutated - see MEMORY_MODEL.md §25 for the
provenance categories governing when such a view may safely be returned or
stored.

---

# 18. Fixed-Size Arrays

A fixed-size array has the syntax:

```text
[T; N]
```

where:

```text
T = element type
N = compile-time element count
```

Example:

```kai
let values: [i32; 4] = [10, 20, 30, 40]
```

The type is:

```text
[i32; 4]
```

The array owns its elements.

Its size is known during compilation, and is part of the type itself -
`[i32; 3]` and `[i32; 4]` are distinct types, as are `[i32; 3]` and
`[u32; 3]`. No runtime length header exists or is needed: length is
purely compile-time structural type information, the same way it is for
any other statically-sized value.

A zero-length array (`[T; 0]`) is a valid, ordinary fixed-size array
type - it is not rejected merely because its length is zero.

**Implementation status (KAI LANGUAGE M7A/M7B/M8B):** M7A established this
as a real semantic type - `[T; N]` annotations resolve to it, array
literals infer it (homogeneous elements only, using the same contextual-
literal-adaptation rules arithmetic already uses - no new implicit-
conversion system), and semantic tooling renders it canonically as
`[T; N]`. M7B (post-alpha.2) implemented native execution for a LOCAL
fixed-size array: literal creation, checked indexed reads/writes (see
"Array Indexing Is Checked" below), and integration with an M6
`for i in start..end` loop all compile to a real native executable. KAI
LANGUAGE M8B (post-alpha.2) then implemented native execution for the
rest (see §18's own "Fixed-Size Arrays at a Function Boundary" subsection
and §19 for the full LANGUAGE semantics, resolved by KAI LANGUAGE M8A):
arrays as a function parameter or return type (a direct LLVM aggregate
argument/result - see §18's "ABI vs. language semantics" below), and
whole-array assignment/copy (`let b = a` / `a = b` for two array-typed
values). Still NOT implemented: slices (`[T]`, still
`Type::unresolved()`), and indexing more than one level into a nested
array (`m[0][1]` - a codegen-only limitation distinct from nested-array
VALUE transport, which works).

The backend representation for a supported element type is LLVM's own
fixed-size aggregate:

```text
[N x T]
```

driven entirely by whatever element types the compiler can already lower
standalone (every integer width, `f32`/`f64`, `bool`, `str`) - `char`
remains unsupported as an array element for the same reason it remains
unsupported standalone.

## Array Index Type

An index expression `xs[index]` accepts any concrete integer type for
`index` - signed (`i8`/`i16`/`i32`/`i64`) or unsigned
(`u8`/`u16`/`u32`/`u64`). A float, `bool`, `char`, `str`, or unit index
is rejected.

## Array Indexing Is Checked (bounds semantics)

For an array of length `N`, `xs[index]` is valid if and only if:

```text
0 <= index < N
```

(for an unsigned `index`, only the upper-bound comparison is needed,
since it can never be negative).

Rules:

1. A compile-time-known out-of-bounds index is rejected at compile time.
2. A dynamic (runtime-determined) index is checked at runtime.
3. A signed dynamic negative index is out-of-bounds.
4. A dynamic out-of-bounds access terminates the program immediately via
   a non-recoverable compiler/runtime trap.
5. This trap is NOT the language's `panic` mechanism.
6. No unwinding or recovery is introduced by this trap.
7. The exact OS signal/process exit code this trap produces is not a
   stable language ABI guarantee.
8. The element address/load/store must never occur before the bounds
   check succeeds.
9. Normal `xs[index]` must never silently lower to an unchecked GEP (or
   equivalent unchecked backend access).
10. A future, explicitly unsafe, unchecked-indexing operation may be
    designed separately - that does not change normal indexing's
    checked semantics.

Documented as of KAI LANGUAGE M7A; implemented in M7B (post-alpha.2) for
a local array binding - a compile-time-constant out-of-bounds index is a
real SemanticError (never an LLVM/backend error), and a dynamic
out-of-bounds index lowers to an actual `llvm.trap` + `unreachable`,
guarded by a real runtime bounds check that always precedes the element
address computation.

## Fixed-Size Arrays at a Function Boundary

**Resolved (KAI LANGUAGE M8A); executable (KAI LANGUAGE M8B):** a
fixed-size array parameter or return type is semantically passed/
returned BY VALUE - the same value-copy rule §19 below states for
`let`/assignment applies identically here.

```kai
fn sum(xs: [i32; 3]) -> i32 {
    return xs[0] + xs[1] + xs[2]
}

fn make() -> [i32; 3] {
    return [1, 2, 3]
}
```

Calling `sum(a)` gives the function its OWN array value - there is no
source-level aliasing between the parameter `xs` and the caller's own
binding `a`; a future mutable copy inside the callee must never be
observable through the caller's own array. `make()`'s returned value has
no borrowed view, no hidden source-level reference, and no lifetime
relationship to `make`'s own locals - `xs` in `let xs = make()` simply
owns the returned value.

Argument/return type matching reuses the EXISTING generic type-
compatibility machinery exactly, never a new implicit-conversion system:

```kai
fn f(xs: [i32; 3]) { ... }

f(a)              // valid only if `a`'s type is exactly [i32; 3]
f([1, 2, 3])      // valid - an inline literal still uses ordinary
                  // contextual literal typing against the declared
                  // parameter type, exactly like any other parameter
```

A wrong length (`[i32; 4]`) or wrong element type (`[u32; 3]`) argument
or return value is rejected via the existing TypeMismatch diagnostic -
no array-specific call/return-checking code exists. A zero-length array
parameter/return type is valid with no special exception. Nested arrays
(`[[i32; 2]; 3]`) as a parameter/return type follow the identical rule
recursively, with no multidimensional-specific redesign needed. Array
parameters remain immutable under KAI's EXISTING (unchanged) parameter-
binding rules - no new mutable-parameter syntax was introduced to
express "by value, no caller aliasing"; that property is already implied
by ordinary value semantics, not by mutability.

**No array decay:** `[T; N]` never implicitly becomes `[T]`, a pointer,
a reference, or any future slice form - at a function boundary or
anywhere else. Slices remain an entirely distinct, still-unimplemented
future type; there is no array-to-slice conversion in KAI 0.1.

**ABI vs. language semantics:** the LANGUAGE guarantees value semantics
as described above. The language does NOT promise a stable, external,
C-compatible ABI for how an array parameter/return value physically
crosses the machine calling convention. KAI LANGUAGE M8B (post-alpha.2)
implemented the initial physical strategy - a DIRECT LLVM aggregate
argument/result (`[T; N]` lowers straight to `[N x T]` as a
`FunctionType` parameter/return type, no `sret`, no `byval`, no hidden
pointer parameter) - as an internal backend implementation detail with no
effect on the semantics above; a different physical strategy could
replace this one in the future without changing any observable KAI
source semantics. This is real, native, executable code as of KAI
LANGUAGE M8B, including an existing array value or an inline array
literal as an argument, both a literal and an existing value as a return
expression, `str`-element arrays, zero-length (`[T; 0]`) arrays, and
nested-array value transport.

---

# 19. Array Copy / Value Semantics

**Resolved (KAI LANGUAGE M8A):** fixed-size arrays are KAI value types.

A fixed-size array `[T; N]` is Copy-like exactly when its element type `T`
is Copy-like:

```text
[T; N] is Copy-like iff T is Copy-like
```

This is a plain, documented language rule - no `Copy` trait, user-defined
trait machinery, or ownership system was introduced to express it. It is
deliberately narrow: it describes KAI 0.1's currently-executable scalar/
value element types (every integer width, `f32`/`f64`, `bool`, and `str`
- see §15 below), and a future non-Copy element type (an owned `String`,
a struct without its own Copy story, ...) may need to refine this rule
once ownership/borrowing exists. That refinement is explicitly NOT
solved here.

Example:

```kai
let first: [i32; 4] = [1, 2, 3, 4]
let second = first
```

`second` receives a real VALUE COPY of `first` - there is no aliasing
relationship between them, no implicit sharing, and no copy-on-write.
Modifying `second` must never be observable through `first`, and (as of
KAI LANGUAGE M8B, post-alpha.2) this is real, native, executable code.

The same value-copy rule governs whole-array ASSIGNMENT:

```kai
mut a: [i32; 3] = [1, 2, 3]
let b: [i32; 3] = [4, 5, 6]
a = b
```

is valid when `a` is mutable and `b` is the EXACT SAME structural array
type - `[i32; 3]` to `[i32; 3]` only, never `[i32; 3]` to `[i32; 4]` or
`[u32; 3]` (no implicit element-by-element conversion between two
already-typed array values - reuses the existing TypeMismatch machinery,
the same way any other type mismatch does). An ARRAY LITERAL may still
use ordinary contextual literal typing regardless (`let xs: [u32; 3] =
[1, 2, 3]` remains valid, since the literal itself - not an existing
typed value - is what adapts) - this is unrelated to, and must never be
generalized into, implicit conversion between two already-typed array
values. Assigning through an immutable binding (`let a = ...; a = b`)
is rejected via the existing immutable-assignment diagnostic, with no
new array-specific error. Self-assignment (`a = a`) is ordinarily valid,
with no special-cased language error for it either.

Zero-length arrays (`[T; 0]`) follow this value-copy rule with no special
exception - copying/assigning a zero-length array is as valid as any
other length.

**Implementation status:** M8A resolved this as LANGUAGE semantics; KAI
LANGUAGE M8B (post-alpha.2) implemented it as real, native, executable
code - whole-array initialization, assignment, and self-assignment all
compile to a real native executable, via an ordinary
alloca/GEP-or-load/store LLVM lowering with no aliasing introduced at
any point. See §18 above (function parameters/returns, also value
semantics, same implementation-status split) and
COMPILER_ARCHITECTURE.md's own M8A/M8B note.

---

# 20. Slices

A slice is a borrowed view into a contiguous sequence of elements.

Slices do not own their elements. This is the fundamental distinction
from a fixed-size array (§18 above), which DOES own its N elements
inline: a slice and an array are separate, non-interchangeable types.
`[T]` (bare slice syntax) never resolves to the Array semantic type, and
still has no semantic Type representation at all as of KAI LANGUAGE M7A
- it remains explicitly deferred, unlike `[T; N]`.

Immutable slice:

```text
&[T]
```

Mutable slice:

```text
&mut [T]
```

Example:

```kai
fn sum(values: &[i32]) -> i32 {
    ...
}
```

The function reads the elements but does not own them.

Mutable example:

```kai
fn reset(values: &mut [i32]) {
    ...
}
```

The function may modify the underlying elements.

---

# 21. Unsized Slice Type

The type:

```text
[T]
```

represents the underlying slice type conceptually but cannot normally exist directly as a local value.

Valid:

```text
&[i32]
&mut [i32]
```

Not valid as an independent variable type:

```text
[i32]
```

This avoids ambiguity between:

* fixed arrays
* borrowed slices
* dynamically owned collections

Earlier experimental KAI examples that used:

```kai
fn sum(values: [i32])
```

should eventually be updated to:

```kai
fn sum(values: &[i32])
```

when the function only reads the sequence.

---

# 22. Buffer<T>

KAI uses:

```text
Buffer<T>
```

as the initial dynamically allocated contiguous collection type.

Example:

```kai
let values = Buffer<i32>(1024)
```

`Buffer<T>`:

* owns its memory
* has runtime length
* stores contiguous elements
* uses Move semantics
* is automatically destroyed when ownership ends

Example:

```kai
let first = Buffer<i32>(1024)
let second = first
```

Ownership moves to `second`.

---

# 23. Buffer and Slices

A Buffer may expose borrowed slices.

Example direction:

```kai
fn process(values: &[f32]) {
    ...
}

let data = Buffer<f32>(1024)

process(&data)
```

This should not transfer ownership.

A mutable Buffer may expose a mutable slice:

```kai
fn normalize(values: &mut [f32]) {
    ...
}

mut data = Buffer<f32>(1024)

normalize(&mut data)
```

The exact standard library conversion mechanism will be finalized later.

---

# 24. Structs

User-defined product types use:

```kai
struct
```

Example:

```kai
struct User {
    id: u64
    name: String
    active: bool
}
```

Fields require explicit types.

Type inference is not allowed in struct declarations.

Invalid:

```kai
struct User {
    id
    name
}
```

---

# 25. Struct Construction

Initial proposed syntax:

```kai
let user = User {
    id: 42
    name: String("Jaret")
    active: true
}
```

Every required field must be initialized.

KAI 0.1 should not silently assign default values to missing fields.

Invalid:

```kai
let user = User {
    id: 42
}
```

unless a future explicit default mechanism exists.

---

# 26. Struct Field Access

Fields use dot syntax.

Example:

```kai
print(user.id)
print(user.name)
```

Mutating a field requires the containing value to be mutable.

Invalid:

```kai
let user = User {
    ...
}

user.active = false
```

Valid:

```kai
mut user = User {
    ...
}

user.active = false
```

---

# 27. Struct Ownership

User-defined structs use Move semantics by default.

This remains true even when all fields are individually Copy.

Example:

```kai
struct Point {
    x: f32
    y: f32
}

let first = Point {
    x: 1.0
    y: 2.0
}

let second = first
```

Initial KAI semantics should treat `first` as moved unless the type is explicitly declared Copy through a future mechanism.

This rule is intentionally conservative.

It avoids automatically changing a type's ownership semantics merely because one of its fields changes.

Future KAI versions may provide explicit Copy declarations.

---

# 28. Explicit Copy Types

Future versions may allow types to explicitly declare Copy behavior.

Possible direction:

```kai
copy struct Point {
    x: f32
    y: f32
}
```

or through a future trait system.

The exact syntax is not finalized.

A type may only implement Copy if all owned fields can safely be copied.

KAI-CC must verify this rule.

---

# 29. Enums

KAI will support sum types using:

```kai
enum
```

Basic example:

```kai
enum Direction {
    North
    South
    East
    West
}
```

Usage:

```kai
let direction = Direction.North
```

Enums may eventually contain associated data.

Example:

```kai
enum ParseError {
    InvalidCharacter(char)
    InvalidNumber(String)
    UnexpectedEnd
}
```

Exact syntax may evolve.

---

# 30. Enum Ownership

Enums use Move semantics by default.

If an enum variant owns a Move value, the enum owns that value.

Example:

```kai
enum Message {
    Text(String)
    Quit
}
```

A `Message.Text` value owns its `String`.

Moving the Message also moves the contained String.

---

# 31. Option<T>

KAI should not use `null` as the ordinary representation of a missing value.

Instead, optional values should eventually use:

```text
Option<T>
```

Conceptually:

```kai
enum Option<T> {
    Some(T)
    None
}
```

Example:

```kai
fn find_user(id: u64) -> Option<User> {
    ...
}
```

This makes absence visible in the function signature.

---

# 32. No Implicit Null

KAI does not allow arbitrary values to silently contain null.

This should not exist:

```kai
let user: User = null
```

If a value may be absent, its type must communicate that possibility.

Example:

```text
Option<User>
```

This improves:

* static safety
* API readability
* compiler analysis
* AI-agent reasoning

---

# 33. Result<T, E>

Recoverable failures use:

```text
Result<T, E>
```

Conceptually:

```kai
enum Result<T, E> {
    Ok(T)
    Err(E)
}
```

Example:

```kai
fn load(path: str) -> Result<String, IOError> {
    ...
}
```

`Result` follows normal ownership rules.

If `T` or `E` owns resources, the Result owns those resources.

Detailed error semantics are defined in:

```text
ERROR_MODEL.md
```

---

# 34. Generics

Generics are a planned core language capability.

Proposed syntax:

```kai
struct Box<T> {
    value: T
}
```

Generic functions:

```kai
fn identity<T>(value: T) -> T {
    return value
}
```

Generic types are necessary for abstractions such as:

```text
Buffer<T>
Option<T>
Result<T, E>
```

The first compiler milestone does not need complete user-defined generic support.

Built-in generic types may initially receive special compiler handling.

---

# 35. Generic Monomorphization

The initial long-term direction is for generic code to support compile-time specialization.

Conceptually:

```kai
identity<i32>(...)
identity<f64>(...)
```

may generate specialized native implementations.

This approach provides predictable performance but can increase binary size.

The exact implementation strategy will be decided when generic support is implemented.

---

# 36. Traits

Traits are planned as KAI's primary mechanism for shared behavior and generic constraints.

Possible future syntax:

```kai
trait Display {
    fn display(self: &Self) -> String
}
```

Implementation:

```kai
impl Display for User {
    fn display(self: &Self) -> String {
        ...
    }
}
```

Potential core traits include:

```text
Copy
Clone
Display
Equal
Order
Error
```

Trait syntax and semantics are not part of the first compiler milestone.

---

# 37. No Class Inheritance

KAI does not plan to use traditional class inheritance as a core abstraction mechanism.

KAI should prefer:

* structs
* composition
* enums
* traits

over deep inheritance hierarchies.

This keeps type relationships easier to inspect and reason about.

---

# 38. Equality

Equality operators:

```text
==
!=
```

require both operands to have the exact same concrete type, and that type
must be one of:

* a signed integer type
* an unsigned integer type
* a floating-point type
* `bool`
* `char`

Result type:

```text
bool
```

Example:

```kai
let a: i32 = 10
let b: i32 = 20

if a == b {
    ...
}
```

Comparing different concrete types is rejected, including different
concrete numeric types:

```kai
let a: i32 = 10
let b: i64 = 10

if a == b {
    ...
}
```

is invalid - `i32` and `i64` are different concrete types, and there is no
implicit numeric conversion. The same applies to comparing an integer type
against a floating-point type, or a primitive against an unrelated type such
as `String`.

The unit type (`()`) is not part of the current committed equality domain:

```kai
if () == () {
    ...
}
```

is not supported by the current KAI 0.1 semantic subset. This reflects what
is currently committed, not a claim that unit values can never become
comparable in a future language version.

No implicit conversion occurs for equality in either direction.

---

# 39. Ordering

Ordering operators:

```text
<
<=
>
>=
```

require both operands to have the exact same concrete numeric type (a
signed integer type, an unsigned integer type, or a floating-point type).

Result type:

```text
bool
```

No implicit conversion occurs: comparing two different concrete numeric
types (e.g. `i32` against `i64`, or `i32` against `f32`) is rejected, the
same as for arithmetic (see "Arithmetic Operators").

`bool` and `char` do not support ordering in KAI 0.1.

Future user-defined types may provide ordering through traits.

The compiler must not invent arbitrary ordering for structs or enums.

## Comparison and Equality Do Not Inherit Outer Context

Unlike arithmetic and modulo (see "Arithmetic Operators"), a comparison or
equality expression's own result type is always `bool`, never the operand
type - so an outer expected type never flows down into a comparison's or
equality's operands, even when both operands are literals:

```kai
let x: i64 = 1 < 2
```

does not cause `1` and `2` to become `i64` merely because the whole
declaration expects `i64`. `1 < 2` has type `bool` regardless of `x`'s
annotation, so this is a type mismatch (`i64` expected, `bool` found) - not
a literal-adaptation success. The same applies to `==`/`!=`.

A literal operand may still adopt type information from a *fixed* sibling
operand within the same comparison or equality expression (e.g. `x < 1`
where `x: i64` types the `1` as `i64`) - only the whole-expression outer
context is excluded, not sibling context.

---

# 40. Type Aliases

KAI may eventually support aliases.

Possible syntax:

```kai
type UserId = u64
```

This would improve API readability without necessarily creating a distinct runtime type.

Example:

```kai
fn get_user(id: UserId) -> Option<User>
```

Type aliases are not required for the first compiler milestone.

---

# 41. Distinct Newtypes

Future KAI versions may also support lightweight wrapper types when semantic distinction is required.

Example concept:

```kai
struct UserId {
    value: u64
}
```

This would prevent accidentally mixing:

```text
UserId
OrderId
ProductId
```

even if all are represented internally using `u64`.

---

# 42. Function Types

Functions are not required to be first-class values in the earliest KAI implementation.

Future versions may support function types such as:

```text
fn(i32, i32) -> i32
```

This will be important for:

* callbacks
* iterators
* functional utilities
* framework APIs

Closure semantics require separate ownership design and are intentionally postponed.

---

# 43. Never Type

KAI will eventually require a type representing computations that never return normally.

Possible examples:

```kai
fn fatal(message: str) -> Never {
    panic(message)
}
```

The exact spelling is not finalized.

Candidates include:

```text
Never
never
!
```

This decision should be made when control-flow analysis requires it.

---

# 44. Type Inference Boundaries

KAI should infer types locally but avoid requiring global inference across large portions of a project.

Inference should primarily operate within:

* expressions
* local variables
* generic calls

KAI should not require an agent or compiler to infer public API signatures from implementation bodies.

This is intentionally invalid:

```kai
pub fn calculate(a, b) {
    return a + b
}
```

Public function signatures must be explicit.

---

# 45. Public API Explicitness

Public interfaces should expose enough type information to understand them without reading their implementation.

Example:

```kai
pub fn load_user(id: u64) -> Result<User, DatabaseError>
```

From this signature alone, a human or AI agent can determine:

* input type
* output type
* possible failure type
* ownership transfer direction

This is a core KAI design principle.

---

# 46. Type Information for AI Tooling

KAI-CC should make inferred and declared type information queryable.

Possible future command:

```text
kai type src/main.kai:12:8
```

Possible output:

```text
expression:
    load_user(id)?

type:
    User

source expression type:
    Result<User, DatabaseError>

after propagation:
    User
```

Another example:

```text
kai inspect User
```

Possible output:

```text
type: struct
ownership: Move

fields:
    id: u64
    name: String
    active: bool

defined:
    src/user.kai:4
```

---

# 47. Machine-Readable Type Information

Semantic tooling should eventually expose structured type information.

Example:

```json
{
    "kind": "struct",
    "name": "User",
    "ownership": "move",
    "fields": [
        {
            "name": "id",
            "type": "u64"
        },
        {
            "name": "name",
            "type": "String"
        },
        {
            "name": "active",
            "type": "bool"
        }
    ]
}
```

AI agents should not need to parse formatted compiler text to understand type relationships.

---

# 48. Type Errors

Type diagnostics should explain both:

* expected type
* actual type

Example:

```text
error[E0301]: incompatible types

 --> src/main.kai:8:20

let age: i32 = "twenty"
               ^^^^^^^^^

expected:
    i32

found:
    str
```

When possible, diagnostics should also identify whether an explicit conversion exists.

---

# 49. No Dangerous Automatic Fixes

KAI-CC should not automatically suggest lossy conversions unless clearly requested.

Example:

```kai
let value: i32 = some_f64
```

The compiler should not blindly suggest:

```kai
i32(some_f64)
```

without indicating that fractional information may be lost.

Structured diagnostics should communicate conversion consequences.

---

# 50. ABI Representation

The language type system should not expose LLVM implementation details.

For example:

```text
i32
```

is a KAI language type.

It may lower to an LLVM `i32`, but LLVM is an implementation detail of KAI-CC.

Likewise:

```text
String
Buffer<T>
Result<T, E>
```

must have KAI-defined semantics independent of their LLVM representation.

This allows future compiler backends to implement the same language.

---

# 51. Platform-Dependent Types

KAI 0.1 should avoid making ordinary source code depend unnecessarily on platform-specific integer widths.

A future type such as:

```text
usize
isize
```

may be introduced for memory indexing and native pointer-sized operations.

If introduced:

```text
usize
```

would match the target architecture's pointer width.

Examples:

```text
32-bit target -> u32-like width
64-bit target -> u64-like width
```

This decision should be finalized when arrays and memory indexing require it.

---

# 52. KAI 0.1 Initial Type Scope

The first usable compiler does not need to implement the entire type system.

Initial required types:

```text
i32
i64
u32
u64
f32
f64
bool
char
()
```

Initial language capabilities:

* literals
* local inference
* explicit function parameter types
* explicit function return types
* arithmetic type checking
* comparison type checking
* assignment type checking
* basic function calls

Then incrementally:

```text
str, String (future)
arrays
slices
Buffer<T>
structs
enums
Result<T, E>
Option<T>
generics
traits
```

---

# 53. KAI 0.1 Restrictions

KAI 0.1 will initially avoid:

* implicit numeric conversions
* null
* class inheritance
* union types
* intersection types
* structural typing
* arbitrary runtime reflection
* dependent types
* implicit dynamic typing
* automatic boxing
* user-defined operator overloading
* complex generic specialization
* higher-kinded types
* first-class lifetimes
* implicit deep copies

These features may be considered individually if real KAI programs justify them.

---

# 54. Initial Type Hierarchy

KAI does not require a universal base type such as:

```text
Object
Any
```

for all values.

Primitive and user-defined values do not automatically inherit from a common runtime object.

This avoids mandatory:

* heap allocation
* virtual dispatch
* runtime type metadata

Dynamic behavior should be introduced only where explicitly required.

---

# 55. Reflection

Full runtime reflection is not part of KAI 0.1.

The compiler itself, however, should maintain rich semantic type information.

This distinction is important:

```text
program runtime reflection
```

and:

```text
compiler semantic introspection
```

are different capabilities.

KAI strongly prioritizes compiler introspection.

For example:

```text
kai inspect User
```

should be possible without requiring every KAI binary to contain a heavyweight reflection runtime.

---

# 56. Token Efficiency

The type system should balance explicitness and source-code compactness.

For example:

```kai
let count = 10
```

is preferred over:

```kai
let count: i32 = 10
```

when the type is obvious locally.

However:

```kai
fn process(data: &Buffer<f32>) -> Result<Report, ProcessError>
```

should remain explicit because function signatures act as semantic boundaries.

KAI optimizes for:

> minimal repeated information, not minimal information.

---

# 57. Predictability Rule

KAI should avoid situations where the same expression changes meaning based on large amounts of distant context.

Type inference should remain local and understandable.

A human or AI agent should usually be able to determine the type of an expression by inspecting:

* the expression
* nearby declarations
* relevant function signatures

without analyzing the complete project.

---

# 58. Summary of Core Type Rules

KAI initially follows these rules:

```text
Primitive scalar values
    -> Copy

Resource-owning values
    -> Move

Local variables
    -> type inference allowed

Function parameters
    -> explicit type required

Function return values
    -> explicit type required

No-value functions
    -> return ()

Numeric conversion
    -> explicit

Missing values
    -> Option<T>, not null

Recoverable errors
    -> Result<T, E>

Owned text
    -> String (future)

Text view (read-only)
    -> str

Fixed array
    -> [T; N]

Immutable slice
    -> &[T]

Mutable slice
    -> &mut [T]

Dynamic contiguous storage
    -> Buffer<T>

User-defined structs
    -> Move by default
```

---

# 59. Open Questions

The following decisions remain intentionally unresolved:

* exact implementation of `String`
* exact `String` to `str` coercion rules
* whether `usize` and `isize` enter KAI 0.1
* exact runtime overflow implementation
* syntax for explicit wrapping arithmetic
* whether arrays automatically implement Copy
* explicit Copy declaration syntax
* enum payload syntax
* exact pattern matching syntax
* implementation strategy for built-in generics
* generic constraints
* trait syntax
* trait object / dynamic dispatch model
* function pointer syntax
* closure syntax
* `Never` type spelling
* type alias syntax
* compile-time constants
* user-defined conversion mechanisms
* operator overloading policy
* FFI type mapping
* ABI stability guarantees

These questions should remain open until implementation or real KAI programs require a decision.

---

# 60. Core Type Rule

KAI should expose enough type information to make behavior predictable while avoiding information that the compiler can safely infer locally.

A type should communicate not only what data a value contains, but also enough information for humans, tools, and AI agents to reason about ownership, mutation, failure, and valid operations without inspecting unnecessary implementation details.

---

# 61. Arithmetic Operators

Arithmetic operators:

```text
+
-
*
/
```

require both operands to have the exact same concrete numeric type - a
signed integer type, an unsigned integer type, or a floating-point type.

Result type:

```text
same as the operand type
```

There is no implicit numeric conversion:

```kai
let a: i32 = 1
let b: i32 = 2
let c = a + b   // i32
```

```kai
let x: f32 = 1.0
let y: f32 = 2.0
let z = x / y   // f32
```

but:

```kai
let a: i32 = 1
let b: i64 = 2
let c = a + b   // error: different concrete types
```

```kai
let a: i32 = 1
let b: f32 = 2.0
let c = a + b   // error: an integer type and a floating-point type are never
                // the same concrete type
```

This document does not commit to a runtime overflow implementation beyond
what "Integer Overflow" above already states.

## Operand Context

When one side of an arithmetic expression is an integer or floating-point
literal (or a parenthesized/negated literal, or itself made up only of such
literals), that literal may adopt the concrete numeric type of the other,
fixed-type side - regardless of which side of the operator it appears on:

```kai
fn f(x: i64) {
    let a = x + 1
    let b = 1 + x

    let c = x + (1 + 2)
    let d = (1 + 2) + x
}
```

`a`, `b`, `c`, and `d` are all `i64`: the literal `1` (and the purely
literal expression `1 + 2`) adopts `x`'s type in every case, independent of
source order. This is contextual literal typing (see "Integer Literals" and
"Floating-Point Literals" above), not a numeric conversion - the same
cross-family restriction still applies, so an integer literal added to an
`f64` value does not become `f64`:

```kai
fn f(x: f64) {
    let y = x + 1   // error: `1` stays an integer literal (i32 by default),
                     // it does not adopt f64
}
```

This operand-context behavior is specific to operators whose successful
result type equals their operand type (arithmetic and modulo). It does not
apply to comparison, equality, or logical operators - see "Comparison and
Equality Do Not Inherit Outer Context" under "Ordering" for why.

---

# 62. Modulo

The modulo operator:

```text
%
```

is integer-only: it requires both operands to have the exact same concrete
integer type (signed or unsigned).

Result type:

```text
same integer type
```

Examples:

```kai
let a: i32 = 7
let b: i32 = 3
let c = a % b     // i32
```

```kai
let a: u64 = 7
let b: u64 = 3
let c = a % b     // u64
```

Floating-point operands are rejected:

```kai
let a: f32 = 7.0
let b: f32 = 3.0
let c = a % b     // error: modulo requires integer operands
```

The operand-context behavior described under "Arithmetic Operators" applies
identically to modulo.

---

# 63. Unary Negation

Unary negation:

```text
-x
```

is valid for:

* signed integer types
* floating-point types

Result type:

```text
same as the operand type
```

Unary negation is not valid on an unsigned integer value:

```kai
fn f(x: u8) {
    let y = -x   // error
}
```

This general rule is distinct from negative-literal contextual typing/range
fitting, which happens at compile time against a literal's exact value
rather than against a general expression's type:

```kai
let x: i8 = -128   // valid: -128 fits i8
```

is valid because `-128` is a single compile-time-known literal value that
fits within `i8`'s range - not because `i8` supports unary negation on an
arbitrary unsigned-like value. See "Integer Literals" above for contextual
literal typing.

---

# 64. Unary Logical Not

The unary logical-not operator:

```text
!expr
```

requires a `bool` operand and returns `bool`:

```kai
let flag = true
let inverted = !flag   // bool
```

KAI does not provide implicit truthiness: `!expr` is rejected for any
concrete non-`bool` operand, including integers:

```kai
let value = 1
let inverted = !value   // error
```

---

# 65. Logical Binary Operators

The logical binary operators:

```text
&&
||
```

require both operands to be `bool` and return `bool`:

```kai
let a = true
let b = false
let both = a && b    // bool
let either = a || b   // bool
```

As with unary logical-not, KAI does not provide implicit truthiness: a
non-`bool` operand on either side is rejected, even if the other operand is
`bool`.

---

# 66. Function Call Argument Checking

Calling an ordinary user-defined function checks each argument against its
corresponding declared parameter type, positionally:

```kai
fn take(value: i64) {}
```

There is no implicit conversion:

```kai
let x = 10   // x is i32
take(x)      // error: i64 expected, i32 found
```

This is ordinary type checking at a call site - not overload resolution, and
not a claim that function names carry a first-class value type (see
"Function Values Are Not First-Class" below).

Literal and expression contextual typing (see "Integer Literals",
"Floating-Point Literals", and "Arithmetic Operators" above) applies in
argument positions exactly as it does in an explicitly typed declaration:

```kai
take(10)       // the integer literal is typed directly as i64
take(1 + 2)    // the pure arithmetic expression may be typed as i64
```

Neither is an implicit conversion - the literal/expression is typed as
`i64` directly, the same way `let x: i64 = 10` types its initializer.

The already-committed cross-family restriction is preserved: an integer
literal never adopts a floating-point parameter type, and a
floating-point literal never adopts an integer parameter type.

```kai
fn take(value: f64) {}

take(1)     // error: value stays an integer literal, not f64
take(1.0)   // valid
```

A call must provide the number of arguments the function declares - too
few or too many are both errors. Every argument that is actually present
in the call is still checked independently, so a call can report more than
one distinct problem at once (e.g. two differently-typed arguments, or an
argument count error alongside an unrelated error inside one of the
arguments).

---

# 67. Function Call Result Type

A successfully checked call to a user function has the function's declared
return type:

```kai
fn get() -> i64 {}

let x = get()   // x is i64
```

A function with no explicit return annotation returns the unit type:

```kai
fn do_work() {}

let x = do_work()   // x is ()
```

A call's result type participates normally in later expression typing,
exactly like any other value:

```kai
fn get() -> i64 {}

let x = get() + 1   // x is i64
```

No first-class `Function` type is introduced by any of this - a call
expression has the function's *return* type, never a type describing the
function itself.

---

# 68. Function Values Are Not First-Class

KAI 0.1 does not commit first-class function values. A function name being
usable as the direct target of a call:

```kai
f()
```

does not mean `f` itself has a semantic function-value type - only the
call expression `f()` has a type (see "Function Call Result Type" above).

Parenthesized grouping around a direct function call target is supported,
since parentheses are ordinary grouping syntax everywhere else in KAI:

```kai
(f)()
(((f)))()
```

This is not a step toward first-class functions: it is the same grouping
behavior parentheses already have around any other expression, applied to
a name that happens to be a function. An arbitrary expression is not
assumed to produce a callable value merely because it appears in a call
position.

---

# 69. Non-Callable Expressions

KAI rejects a call whose target expression is known to have a concrete,
non-callable type:

```kai
let x = 1
x()   // error: i32 is not callable

1()   // error: i32 is not callable
```

This is a narrow check against a *known* concrete type - it is not the
introduction of first-class functions, methods, callable structs,
closures, or function pointers. None of those are implemented or
committed in KAI 0.1.

Correspondingly, calling an expression whose own callability is not yet
modeled at all is not treated as an error:

```kai
obj.method()
values[index]()
result?()
```

Member access, indexing, and error propagation do not yet have committed
semantics of their own (see the relevant sections above), so a call
targeting one of them is neither accepted nor rejected - it is simply not
yet checked. Method-call and function-value semantics remain future work.

---

# 70. Binding Mutability and Reassignment

```kai
let x = value
```

creates an immutable binding. Reassigning it is a semantic error:

```kai
let x = 1
x = 2   // error
```

```kai
mut x = value
```

creates a mutable binding, which may be reassigned when the assigned value
is type-compatible with it (see "Assignment Type Checking" below):

```kai
mut x = 1
x = 2   // valid
```

Function parameters are immutable bindings:

```kai
fn f(x: i64) {
    x = 1   // error
}
```

Binding mutability (`let` vs. `mut`) is a distinct concept from reference/
referent mutability (`&T` vs. `&mut T`, see "Memory Model" §12 for the
committed relationship between the two where borrowing is concerned).
Assignment through a reference, and mutation reachable only through a
`&mut` borrow, are separate, not-yet-committed concerns.

---

# 71. Assignment Targets

The current KAI 0.1 semantic subset checks assignment to an identifier
that resolves to a local variable or a function parameter, optionally
wrapped in grouping parentheses:

```kai
x = value
(x) = value
(((x))) = value
```

Ordinary value-producing expressions are not assignment targets:

```kai
1 = value        // error
(a + b) = value  // error
f() = value      // error
```

A function or prelude name is not an assignment location either - it does
not name a variable-like binding at all:

```kai
fn foo() {}

foo = 1   // error
```

Assignment to a field or an element:

```kai
object.field = value
values[index] = value
```

is not yet part of the checked subset. This is a current semantic-model
limitation, not a statement that these forms are permanently invalid KAI
syntax - their eventual assignability depends on field semantics,
array/slice/`Buffer` semantics, and reference/ownership mutability, none
of which are committed yet (see "Memory Model" and the `Buffer<T>`/slice
sections above). Until those are designed, such an assignment is simply
not yet checked, one way or the other.

---

# 72. Assignment Type Checking and Result

The right-hand side of an assignment to a binding of known type is checked
against that binding's type, the same way an explicitly typed declaration
would be. There is no implicit conversion:

```kai
mut x: i64 = 0

x = 1        // valid: the integer literal is typed directly as i64
x = 1 + 2    // valid: the arithmetic expression may be typed as i64
x = true     // error: bool is not i64
```

The already-committed cross-family restriction still applies: an integer
literal assigned to a floating-point-typed binding does not adapt to it,
and vice versa (see "Integer Literals" and "Floating-Point Literals"
above).

An assignment expression itself has the unit type, `()` - never the
assigned value's type:

```kai
mut x = 0
let y = (x = 1)   // y is ()
```

This is intentional, and it has one notable consequence: because the
grammar is right-associative, `x = y = z` parses as `x = (y = z)` - but
since `y = z` itself has type `()`, this is not C-style value-producing
chained assignment. For ordinary, non-unit-typed variables, the outer
assignment will generally fail, because its right-hand side is `()`, not
the assigned value's type.

Since an assignment's type is `()`, it does not become numeric merely
because it appears inside another expression:

```kai
mut x: i64 = 0

(x = 1) + 2   // error: () is not a numeric type
```

---

# 73. Condition Typing

The condition of `if` (and every `else if`) and `while` must have type
`bool`. KAI has no truthiness - no other type is accepted, regardless of
whether it could plausibly be treated as "truthy" in another language:

```kai
if true { ... }        // valid
if 1 < 2 { ... }        // valid: comparison produces bool

if 1 { ... }             // error: i32 is not bool
if 1 + 2 { ... }         // error: arithmetic stays i32, not bool
```

A final `else` has no condition of its own and is unaffected by this rule.

This composes with the rest of the type system without a dedicated
"condition" rule of its own - a condition is simply an ordinary expression
checked for `bool`:

```kai
fn predicate() -> bool { ... }

if predicate() { ... }   // valid: the call has type bool
```

```kai
mut flag: bool = false

if flag = true { ... }   // error: assignment has type (), not bool
```

The second example is rejected for the same reason `(x = 1) + 2` is
rejected above - an assignment's type is `()`, never the assigned value's
type - not because of any special "assignment used as a condition" rule.

This section covers `if` and `while` only - a `for` loop has no
Bool-typed condition at all, so this specific rule never applies to it.
A `for` loop's iterable is type-checked by its own separate rule (KAI
LANGUAGE M6, post-alpha.2): a literal `start..end` integer range is
validated and typed; any other iterable shape is rejected outright.
General iterable/foreach semantics remain a separate, still-deferred
concern.

---

# 74. Return Type Compatibility

Every `return` statement that appears in a function body is checked
against that function's declared return type, the same way a `let`
initializer is checked against an explicit annotation. There is no
implicit conversion:

```kai
fn f() -> i64 {
    return 1
}
```

Contextual literal and expression typing apply in return position exactly
as they do anywhere else:

```kai
fn f() -> i64 {
    return 1 + 2   // the arithmetic expression may be typed as i64
}
```

A bare `return` (with no expression) is checked as though it returned the
unit type, `()`:

```kai
fn f() {
    return       // valid: () matches f's declared (implicit) () return
}

fn f() -> i64 {
    return       // error: expected i64, found ()
}
```

The unit type is an ordinary return type, not a special case - a return
expression is valid whenever its type matches the function's declared
return type, *including* when that type is `()`:

```kai
fn a() {
    return
}

fn b() {
    return ()          // valid: () matches b's declared () return
}

fn do_work() {}

fn c() {
    return do_work()   // valid: do_work() also has type ()
}

fn d() -> () {
    return ()          // valid, for the same reason, with an explicit annotation
}
```

Conversely, a unit-returning function may not return a non-unit value,
whether its unit return is implicit or written explicitly as `-> ()`:

```kai
fn f() {
    return 1   // error: expected (), found i32
}

fn f() -> () {
    return 1   // error: expected (), found i32
}
```

A `return` statement never changes a function's declared return type -
the declaration remains the function's semantic contract regardless of
what its body returns; a mismatching `return` is a body error, exactly
like a mismatching call argument is a call-site error rather than a
change to the callee's signature.

Every `return` in a function is checked independently:

```kai
fn f(x: bool) -> i64 {
    if x {
        return 1
    }

    return 2
}
```

both returns above are checked against `i64` on their own merits.

---

# 75. Return Completeness (All-Paths-Return) Analysis

The rule in "Return Type Compatibility" above checks every `return`
statement that actually appears in a function body against that
function's declared return type. It does **not** check whether a
value-returning function returns at all, or on every possible path
through its body. These are two different checks, and both are now
implemented:

- **Return-statement type compatibility** (`# 74` above) - every
  `return` that appears is checked against the declared return type.
- **Return completeness** (this section) - a function with a concrete,
  non-`()` return type must be structurally unable to fall through to
  the end of its body without reaching a `return`.

Referring to either one by the bare phrase "return validation" remains
ambiguous; name the specific check instead.

A function whose body can fall through to its end produces
`MissingReturn`:

```kai
fn f() -> i64 {
}
```

```kai
fn f(cond: bool) -> i64 {
    if cond {
        return 1
    }
}
```

Both examples above are diagnosed as `MissingReturn`. A function whose
every path reaches a `return` is complete and produces no diagnostic:

```kai
fn f(cond: bool) -> i64 {
    if cond {
        return 1
    } else {
        return 2
    }
}
```

### Distinct from return-statement type compatibility

Return completeness is purely structural: it asks only whether control
can reach the end of the body, never whether a `return` it encounters
along the way has the right type. A function whose only `return`
structurally terminates every path is complete even if that `return`
itself is type-mismatched - `TypeMismatch` is produced, not
`MissingReturn`:

```kai
fn f() -> i64 {
    return true   // TypeMismatch: expected i64, found bool - not MissingReturn
}
```

A bare `return` behaves the same way: it structurally terminates the
path (so it never causes `MissingReturn`) even though it is typed as `()`
and is itself flagged as a mismatch against a non-`()` return type:

```kai
fn f() -> i64 {
    return        // TypeMismatch: expected i64, found () - not MissingReturn
}
```

The two diagnostics are independent and may legitimately coexist when a
function has both problems on different paths:

```kai
fn f(cond: bool) -> i64 {
    if cond {
        return    // TypeMismatch: expected i64, found ()
    }
    // the `false` path reaches the end of the function: MissingReturn
}
```

### Unit-returning functions

A function whose declared return type is `()`, implicit or explicit, has
no completeness requirement - it is valid whether or not every path
returns explicitly:

```kai
fn f() {
}

fn f() -> () {
}

fn f(cond: bool) {
    if cond {
        return
    }
}
```

### Structural `if` / `else if` / `else` rule

An `if` chain is complete only when it has a final `else` and every
branch - the initial `if`, every `else if`, and the final `else` - is
itself complete:

```kai
fn f(cond: bool) -> i64 {
    if cond {
        return 1
    }
}
```

is incomplete (`MissingReturn`) because there is no `else`: control can
always fall out the bottom when `cond` is false. Adding the `else` makes
it complete:

```kai
fn f(cond: bool) -> i64 {
    if cond {
        return 1
    } else {
        return 2
    }
}
```

### Current analysis limitation: no constant-condition reasoning

The current analysis does not evaluate condition expressions at all -
not even literal `true`/`false`. An `if` with no `else` is treated as
incomplete regardless of its condition:

```kai
fn f() -> i64 {
    if true {
        return 1
    }
}
```

is still diagnosed as `MissingReturn` today, even though `true` can never
be false. This is a **limitation of the current analysis, not a KAI
language rule** - a later revision may add constant-condition reasoning
that recognizes this case as complete.

### Current analysis limitation: `while` never proves completeness

A `while` loop is conservatively treated as never proving completeness by
itself, regardless of its condition or body:

```kai
fn f() -> i64 {
    while true {
        return 1
    }
}
```

is diagnosed as `MissingReturn` today, even though `while true` never
exits on its own. As with constant-condition `if`, this is an **accepted
limitation of the current analysis, not the intended final behavior**,
and may be refined once constant-condition and divergence reasoning are
added.

### `for` loops

A `for` loop likewise never proves completeness by itself: its iterable
may execute zero times, so a `return` inside a `for` body does not make
the enclosing path complete without a subsequent `return` after the
loop:

```kai
fn f() -> i64 {
    for i in 0..10 {
        return 1
    }
    // still required: the loop may run zero iterations
}
```

`for`-loop iterable and element type checking are now implemented (KAI
LANGUAGE M6, post-alpha.2 - a literal `start..end` integer range); this
only describes `for`'s effect on return completeness, which is
unaffected by that milestone.

### Not unreachable-code analysis

Return completeness only decides whether a body can fall through to its
end; it does not diagnose code that can never run. Code that follows a
`return` statement is still fully, independently checked, exactly as
before:

```kai
fn f() -> i64 {
    return 1
    let x = undefined_name   // still checked; still an error; not "unreachable"
}
```

KAI does not yet have unreachable-code analysis, constant-condition flow
reasoning, divergence analysis, or a general control-flow graph. Return
completeness is a narrow, purely structural check layered on the
existing AST shape - it is not a byproduct of, and does not require, any
of those.
