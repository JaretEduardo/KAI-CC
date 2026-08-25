# KAI Memory Model

> Status: Experimental Draft
> Target version: KAI 0.1
> Last updated: August 2026

## 1. Overview

KAI uses deterministic resource management without requiring a tracing garbage collector.

The memory model is designed around four primary goals:

* predictable resource lifetime
* prevention of common memory safety errors
* low runtime overhead
* simple semantics for both humans and AI coding agents

KAI takes inspiration from ownership-based systems languages, but does not attempt to reproduce the full complexity of Rust's ownership and lifetime system.

KAI 0.1 intentionally uses a smaller and more restrictive model.

The core concepts are:

* ownership
* copy semantics
* move semantics
* immutable borrowing
* mutable borrowing
* deterministic destruction

---

## 2. Core Principle

Every resource-owning value has exactly one owner at a time.

Example:

```kai
let data = Buffer<i32>(1024)
```

`data` owns the created buffer.

When the owner reaches the end of its lifetime, the resource is automatically released.

```kai
fn process() {
    let data = Buffer<i32>(1024)

    // use data
}

// data is automatically destroyed here
```

KAI does not require:

```kai
free(data)
```

for ordinary managed resources.

---

## 3. Value Categories

KAI values are conceptually divided into two main categories:

* Copy values
* Move values

---

## 4. Copy Values

Small and inexpensive primitive values use copy semantics.

Initial `Copy` types include:

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
```

Example:

```kai
let a = 10
let b = a
```

After the assignment:

```text
a = 10
b = 10
```

Both variables remain valid.

Using `a` does not invalidate it.

```kai
print(a)
print(b)
```

is valid.

---

## 5. Move Values

Resource-owning values use move semantics by default.

Examples may include:

```text
Buffer<T>
String
File
Socket
User-defined resource-owning structs
```

Example:

```kai
let data = Buffer<i32>(1024)
let other = data
```

Ownership moves from `data` to `other`.

Conceptually:

```text
data
  |
  | move
  v
other
```

After the move:

```kai
print(other)
```

is valid.

However:

```kai
print(data)
```

is invalid.

The compiler should report a use-after-move error.

Example diagnostic:

```text
error[E0401]: use of moved value `data`

 --> src/main.kai:7:11

let other = data
            ---- ownership moved here

print(data)
      ^^^^ value used after move
```

---

## 6. Explicit Cloning

KAI must not silently perform expensive deep copies of resource-owning values.

If a developer needs an independent copy, the operation must be explicit.

Example:

```kai
let data = Buffer<i32>(1024)
let other = data.clone()

print(data)
print(other)
```

Both values remain valid because `clone()` explicitly creates another owned resource.

The exact mechanism used to expose cloning may evolve when KAI introduces traits.

KAI 0.1 treats cloning as an explicit operation.

---

## 7. Function Arguments

Function signatures communicate ownership behavior.

There are three important forms:

```text
T
&T
&mut T
```

They mean:

```text
T       -> ownership is passed to the function
&T      -> temporary read-only borrow
&mut T  -> temporary mutable borrow
```

This rule should allow a developer or AI agent to understand the ownership behavior of a function directly from its signature.

---

## 8. Passing by Value

Passing a `Copy` type by value copies the value.

Example:

```kai
fn square(value: i32) -> i32 {
    return value * value
}

fn main() {
    let number = 10

    print(square(number))
    print(number)
}
```

`number` remains valid.

For a Move type, passing by value transfers ownership.

```kai
fn consume(data: Buffer<i32>) {
    print(data.len)
}

fn main() {
    let data = Buffer<i32>(1024)

    consume(data)

    print(data)
}
```

The final `print(data)` is invalid because ownership was transferred to `consume`.

---

## 9. Immutable Borrowing

An immutable borrow allows temporary read-only access without transferring ownership.

Syntax:

```kai
&T
```

Creating a borrow:

```kai
&value
```

Example:

```kai
fn inspect(data: &Buffer<i32>) {
    print(data.len)
}

fn main() {
    let data = Buffer<i32>(1024)

    inspect(&data)

    print(data)
}
```

`inspect` does not own `data`.

After the function returns, `data` remains valid.

---

## 10. Mutable Borrowing

A mutable borrow allows temporary modification without transferring ownership.

Syntax:

```kai
&mut T
```

Creating a mutable borrow:

```kai
&mut value
```

Example:

```kai
fn reset(data: &mut Buffer<i32>) {
    data[0] = 0
}

fn main() {
    mut data = Buffer<i32>(1024)

    reset(&mut data)

    print(data[0])
}
```

The owner remains responsible for the resource.

---

## 11. Borrowing Rules

KAI 0.1 follows a simple borrowing rule:

> A value may have either multiple active immutable borrows or one active mutable borrow, but not both at the same time.

Valid:

```kai
let first = &data
let second = &data
```

Multiple immutable borrows are allowed.

Invalid:

```kai
let first = &data
let second = &mut data
```

An immutable and mutable borrow may not overlap.

Also invalid:

```kai
let first = &mut data
let second = &mut data
```

Only one mutable borrow may exist at a time.

---

## 12. Mutability

Ownership and mutability are separate concepts.

This:

```kai
let data = Buffer<i32>(1024)
```

creates an immutable binding.

This:

```kai
mut data = Buffer<i32>(1024)
```

creates a mutable binding.

A mutable borrow can only be created from a mutable value.

Invalid:

```kai
let data = Buffer<i32>(1024)

modify(&mut data)
```

Valid:

```kai
mut data = Buffer<i32>(1024)

modify(&mut data)
```

---

## 13. Borrow Lifetimes in KAI 0.1

KAI 0.1 does not expose explicit lifetime syntax.

The following kind of syntax will not exist in KAI 0.1:

```text
'a
'b
T<'a>
&'a T
```

Borrow lifetimes should normally be inferred by the compiler.

KAI 0.1 will deliberately restrict reference usage to keep lifetime analysis simple.

This restriction applies to KAI 0.1 as currently scoped, not as a permanent
promise: explicit lifetime or provenance syntax remains a reserved future
design possibility if real programs demonstrate cases local inference cannot
resolve (for example, a function with more than one view-shaped input
returning a view - see §25's provenance discussion, and §39's open
questions). KAI should prefer local, restricted provenance inference over a
general lifetime system for as long as it remains sufficient.

---

## 14. References Inside Structs

KAI 0.1 does not allow ordinary borrowed references to be stored inside structs.

This is intentionally unsupported:

```kai
struct UserView {
    user: &User
}
```

This restriction prevents KAI 0.1 from requiring explicit lifetime parameters.

Future versions may revisit this rule if a sufficiently simple model can be designed.

---

## 15. Returning Owned Values

Returning an owned value transfers ownership to the caller.

Example:

```kai
fn create_buffer() -> Buffer<i32> {
    let data = Buffer<i32>(1024)

    return data
}

fn main() {
    let data = create_buffer()

    print(data.len)
}
```

Conceptually:

```text
create_buffer
      |
      | ownership
      v
    caller
```

The buffer must not be destroyed when `create_buffer` returns because ownership has moved to the caller.

---

## 16. Returning References

KAI 0.1 should heavily restrict returning borrowed references.

This kind of code must never be valid:

```kai
fn invalid() -> &i32 {
    let value = 10
    return &value
}
```

`value` is destroyed when the function ends, so the returned reference would be invalid.

For the initial language implementation, returning references may be entirely unsupported until safe semantics are formally defined.

---

## 17. Scope-Based Destruction

Owned resources are destroyed when their owning scope ends.

Example:

```kai
fn example() {
    let first = Buffer<i32>(100)

    if true {
        let second = Buffer<i32>(200)

        // second is destroyed here
    }

    // first is destroyed here
}
```

Destruction must be deterministic.

KAI should not depend on a garbage collector deciding when a resource is released.

---

## 18. Resource Cleanup

The ownership model applies to resources beyond heap memory.

For example:

```kai
fn load() {
    let file = File.open("config.kai")

    // use file
}
```

When `file` leaves scope, the file handle should be released automatically.

The same model may eventually apply to:

* files
* sockets
* locks
* GPU buffers
* operating system handles
* custom resources

This allows KAI to use a unified model for resource management.

---

## 19. Destruction Order

Values should be destroyed in reverse declaration order within the same scope.

Example:

```kai
fn example() {
    let first = Resource()
    let second = Resource()
    let third = Resource()
}
```

Destruction order:

```text
third
second
first
```

This behavior must be deterministic.

---

## 20. Struct Ownership

Structs own their fields unless a field type has different semantics defined by the language.

Example:

```kai
struct User {
    id: u64
    name: String
}
```

If `String` is a Move type, then `User` is also a Move type by default.

Example:

```kai
let first = User {
    id: 1
    name: String("KAI")
}

let second = first
```

Ownership of the complete `User` moves to `second`.

`first` becomes invalid.

---

## 21. Copy Structs

A struct may eventually be considered `Copy` when all of its fields are `Copy`.

Example:

```kai
struct Point {
    x: f32
    y: f32
}
```

Because both fields are `Copy`, KAI may allow:

```kai
let a = Point {
    x: 10.0
    y: 20.0
}

let b = a

print(a.x)
print(b.x)
```

The exact mechanism for determining or declaring `Copy` behavior is not finalized for KAI 0.1.

---

## 22. Arrays

KAI distinguishes between fixed-size arrays, borrowed slices, and dynamically allocated buffers.

### Fixed-size array

Proposed syntax:

```kai
[i32; 4]
```

Example:

```kai
let values: [i32; 4] = [10, 20, 30, 40]
```

The size is known at compile time.

---

## 23. Slices

A slice represents a borrowed view into a contiguous sequence.

Proposed syntax:

```kai
&[i32]
```

Example:

```kai
fn sum(values: &[i32]) -> i32 {
    ...
}
```

A slice does not own the underlying elements.

Mutable slices use:

```kai
&mut [i32]
```

Example:

```kai
fn reset(values: &mut [i32]) {
    ...
}
```

---

## 24. Dynamic Buffers

Dynamically allocated sequences will initially use:

```kai
Buffer<T>
```

Example:

```kai
let values = Buffer<i32>(1024)
```

`Buffer<T>` owns its memory and therefore uses move semantics.

Example:

```kai
let first = Buffer<i32>(1024)
let second = first

print(first)
```

The final access is invalid because ownership moved to `second`.

The exact standard library API for `Buffer<T>` is still under design.

---

## 25. Strings

KAI distinguishes two text-related types:

```text
str      Copy, immutable, non-owning UTF-8 text view
String   Move, owned, growable UTF-8 buffer (future - see §24's Buffer<T>
         for the closest existing analogue; not yet implemented)
```

`str`'s runtime shape is conceptually `{ ptr, len }`: a small, fixed-size
descriptor, not a reference and not an unsized type. Passing a `str` by value
copies this descriptor, never the text itself, and never allocates. `len` is
a byte length, never a codepoint or grapheme count.

String literals:

```kai
let language = "KAI"
```

have the concrete type `str`, are backed by static program storage, and
require no explicit manual memory management - this already holds in the
current compiler, not only as a future goal.

`String` will be the owned counterpart, heap-backed with Move semantics and
deterministic scope-based destruction (§17), directly analogous to
`Buffer<T>` (§24). Passing a `String` where `str` is expected is intended to
be an implicit, non-allocating borrow of the `String`'s own buffer once
`String` exists - see the provenance rules below for why such a view cannot
always be returned safely.

### Provenance categories (conceptual design target - not yet implemented)

Because `str` is non-owning, KAI needs a way to reject a `str` that outlives
the data it points at, without requiring the user-visible lifetime syntax
§13 rules out for ordinary KAI 0.1 programs. The intended model classifies
every `str` value into exactly one of three categories:

* **`Static`** - backed by program literal storage. Never expires; safe to
  return or hold indefinitely without restriction.
* **`ExternallyOwned`** - a view whose backing data is owned by something
  outside the current function activation, e.g. a `str` value received as a
  parameter, or (once designed) a future borrowed-owner parameter shape. Safe
  to propagate, including returning it, because the callee never controls
  when that data is destroyed - that responsibility always belonged to the
  caller (or beyond).
* **`LocallyOwned`** - a view into something the *current function*
  owns - including a **by-value `String` parameter** (ownership was moved in,
  so the callee destroys it on return, exactly like a local) or a `String`
  local created inside the function. A view into a `LocallyOwned` value must
  never be returned or stored past the function's own scope, since the
  backing data is destroyed when that scope ends.

This is a deliberately restricted, single-function-local analysis, not a
general lifetime-inference system - and it introduces no new syntax. It is a
**design target for a future compiler milestone**: the current compiler has
no `String` type and no provenance checker of any kind. Every `str` value the
current compiler can produce today is `Static`, because a literal is
currently the only way to construct one - see "Implementation Boundary"
below.

Worked examples of the intended return rule:

```kai
// SAFE - Static provenance, no owner to outlive
fn static_name() -> str {
    return "KAI"
}

// SAFE - single ExternallyOwned(s) input; the return is exactly that input
fn identity(s: str) -> str {
    return s
}

// DEFERRED/REJECTED - two str-shaped inputs; which one the return's
// provenance ties to is ambiguous with no disambiguation mechanism designed
// yet (§39)
fn choose(a: str, b: str) -> str {
    return a
}

// UNSAFE/REJECTED - `s` is owned BY THE CALLEE (pass-by-value transfers
// ownership in) and is destroyed when this function returns; a str view
// into it would dangle
fn bad(s: String) -> str {
    return s.as_str()
}
```

### UTF-8 invariant

Target invariant: every `str` value's bytes are valid UTF-8. **Current
implementation gap:** the lexer preserves ordinary source bytes and decodes
the supported escapes (`\n \r \t \\ \" \0`) exactly, but does not yet perform
full UTF-8 validation - this must be enforced explicitly (most naturally
wherever a `str`/`String` value is constructed from raw bytes) before the
invariant becomes normative; it is not already guaranteed today. A `\0`
(NUL) byte is itself ordinary, valid UTF-8 and does not violate this
invariant on its own.

### Slicing (future)

A future *safe* operation that slices a `str` and returns `str` must preserve
UTF-8 validity - it must never silently produce a `str` whose bytes split a
multi-byte codepoint at an invalid boundary. Raw, unchecked byte access
(which may cross such a boundary) may exist as a separate, explicitly-marked
operation that does not claim to produce a valid `str`. Exact slicing syntax
and failure behavior remain future work (§39).

### Implementation boundary

Until `String` and any borrowed-owner parameter shape exist, no `str` value
constructible by the current language can be anything other than `Static`.
This means a future compiler milestone can safely make `str` a spellable
type annotation, parameter type, and return type (for the `Static` cases
above) **without implementing the general provenance checker described in
this section** - the checker only becomes necessary once `String` (or
another owned/borrowable text source) exists at all.

---

## 26. Heap Allocation

Ordinary KAI code should not require explicit calls equivalent to:

```text
malloc
free
```

Heap-backed standard library types should manage their resources through ownership.

Low-level allocation APIs may eventually be available for systems programming.

Such APIs may require an explicit `unsafe` context.

---

## 27. Unsafe Code

KAI may eventually expose operations that cannot be fully verified by the compiler.

Possible future syntax:

```kai
unsafe {
    // low-level memory operation
}
```

Potential unsafe operations may include:

* raw pointer dereference
* manual allocation
* manual deallocation
* pointer arithmetic
* unchecked memory access
* FFI operations

`unsafe` does not disable all compiler checking.

It only permits explicitly defined operations that cannot otherwise be proven safe.

Unsafe code is not required for the first KAI compiler milestone.

---

## 28. Raw Pointers

Raw pointer syntax is not finalized.

Possible future forms may include:

```text
*const T
*mut T
```

or a KAI-specific alternative.

Raw pointers should not participate in normal ownership guarantees.

They should primarily exist for:

* operating system interfaces
* hardware interaction
* FFI
* advanced systems programming

Raw pointers are not part of KAI 0.1.

---

## 29. Garbage Collection

KAI does not require a tracing garbage collector.

This means ordinary resource lifetime should not depend on periodic GC execution.

Libraries may implement reference counting, arenas, garbage collectors, or other memory strategies when appropriate.

These mechanisms should be library-level abstractions rather than mandatory properties of the language runtime.

---

## 30. Cyclic Data Structures

Ownership-based memory makes cyclic structures more complicated.

KAI 0.1 does not attempt to solve every possible cyclic ownership pattern automatically.

Future standard library mechanisms may include:

* reference-counted pointers
* weak references
* arenas
* object pools

These should not complicate the core memory model.

---

## 31. Compiler Diagnostics

Ownership diagnostics are a first-class part of KAI.

Errors should identify:

* the affected symbol
* where ownership moved
* where a borrow began
* where conflicting access occurred
* what rule was violated

Example:

```text
error[E0401]: use of moved value `data`

 --> src/main.kai:10:11

7 | consume(data)
            ---- ownership moved here

10 | print(data)
           ^^^^ value used after move

`Buffer<i32>` uses move semantics.
```

---

## 32. Borrow Conflict Diagnostic

Example:

```kai
let view = &data

modify(&mut data)
```

Possible diagnostic:

```text
error[E0402]: conflicting borrow of `data`

 --> src/main.kai:8:8

6 | let view = &data
               ----- immutable borrow begins here

8 | modify(&mut data)
             ^^^^^^^^^ mutable borrow requested here

`data` cannot be mutably borrowed while an immutable borrow is active.
```

Machine-readable diagnostics should provide equivalent information.

---

## 33. AI-Agent Considerations

The memory model should be understandable from function signatures without requiring an agent to inspect implementation details.

For example:

```kai
fn inspect(data: &Buffer<f32>)
```

communicates:

```text
does not take ownership
does not modify data
```

While:

```kai
fn modify(data: &mut Buffer<f32>)
```

communicates:

```text
does not take ownership
may modify data
```

And:

```kai
fn consume(data: Buffer<f32>)
```

communicates:

```text
takes ownership
```

This explicit ownership information reduces ambiguity during automated code generation and refactoring.

---

## 34. Semantic Tooling

Future KAI tooling should expose ownership information directly.

Possible commands:

```text
kai inspect data
kai ownership data
kai borrows data
```

Possible output:

```text
symbol: data
type: Buffer<i32>
owner: main
mutable: true

ownership:
  created: src/main.kai:4
  moved: no

active borrows:
  immutable: 1
  mutable: 0
```

This information may also be exposed through a structured API for AI coding agents.

---

## 35. KAI 0.1 Restrictions

To keep the first implementation manageable, KAI 0.1 will deliberately NOT support:

* explicit lifetime parameters
* references stored in ordinary structs
* arbitrary long-lived references
* raw pointers
* pointer arithmetic
* manual `free`
* custom allocators
* user-defined destructors
* cyclic ownership analysis
* reference counting built into the language
* garbage collection
* unsafe blocks
* returning arbitrary borrowed references

These features may be revisited individually in later versions.

---

## 36. Initial Compiler Requirements

The first implementation should be capable of detecting:

* use after move
* modification of immutable bindings
* invalid mutable borrows
* overlapping mutable borrows
* conflicting mutable and immutable borrows
* returning references to local values
* basic ownership transfer through function calls

More advanced lifetime analysis can be introduced incrementally.

---

## 37. Example

A complete ownership example:

```kai
fn create() -> Buffer<i32> {
    let data = Buffer<i32>(100)

    return data
}

fn inspect(data: &Buffer<i32>) {
    print(data.len)
}

fn modify(data: &mut Buffer<i32>) {
    data[0] = 42
}

fn consume(data: Buffer<i32>) {
    print(data.len)
}

fn main() {
    mut data = create()

    inspect(&data)

    modify(&mut data)

    inspect(&data)

    consume(data)

    // ERROR:
    // data has been moved into consume()
    //
    // print(data)
}
```

Ownership flow:

```text
create()
   |
   | return ownership
   v
 data
   |
   +---- &data ------> inspect()
   |                    |
   |<-------------------+
   |
   +---- &mut data --> modify()
   |                    |
   |<-------------------+
   |
   +---- &data ------> inspect()
   |                    |
   |<-------------------+
   |
   | move
   v
consume()
   |
   v
destroy data
```

At no point does the program require explicit deallocation.

---

## 38. Design Goals Summary

KAI's memory model should provide:

* deterministic destruction
* no mandatory garbage collector
* explicit ownership transfer
* inexpensive copy semantics for primitive values
* move semantics for resources
* immutable borrowing
* mutable borrowing
* strong compile-time diagnostics
* minimal hidden behavior
* no explicit lifetime syntax in ordinary KAI 0.1 programs

---

## 39. Open Questions

The following decisions remain intentionally unresolved:

* how multi-input `str`-view return provenance should be disambiguated, if
  ever (see §25's `choose` example)
* whether `str` views may ever be stored (struct fields, collections)
* whether explicit provenance/lifetime syntax will ever be needed for cases
  local inference cannot resolve (§13)
* exact `str` slicing rules (safe UTF-8-boundary-preserving vs. raw/unchecked)
* whether mutable in-place string views will ever exist
* exact relationship between `str`/`String` and a future generic
  `Buffer<T>`/slice-of-T view design
* whether user-defined structs automatically become `Copy`
* how cloning will integrate with future traits
* whether partial moves from structs will be allowed
* exact borrow lifetime inference rules for non-string references
* whether references may be returned in later versions
* custom destructor syntax
* allocator APIs
* arena support
* reference-counted standard library types
* exact raw pointer syntax
* interaction between ownership and future async/concurrency features

These questions should remain open until real KAI programs demonstrate a need for them.

---

## 40. Core Memory Rule

KAI should make ownership visible enough that a human or AI agent can predict what happens to a resource by reading the surrounding code.

Memory safety should come primarily from simple and deterministic language rules rather than hidden runtime behavior.
