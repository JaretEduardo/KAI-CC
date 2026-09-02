# KAI Design Questions

## Syntax

- Should final function expressions imply return?
- Should ranges support `..=`?
- Should compound operators such as `+=` exist?
- Should blocks be expressions?

## Types

- What is the default integer type?
- Are strings immutable? (`str` itself is always immutable; whether/how
  `String` exposes in-place mutation is still open)
- How should multi-input `str`-view return provenance be disambiguated, if
  ever (e.g. `fn choose(a: str, b: str) -> str`)? See MEMORY_MODEL.md §25.
- Will `str` views ever be storable (struct fields, collections)?
- Exact `str` slicing rules (safe, UTF-8-boundary-preserving vs. raw/unchecked)?
- Will KAI ever support mutable in-place string views?
- Exact relationship between `str`/`String` and a future generic
  `Buffer<T>`/slice-of-T view design?

## Memory

- Are arguments passed by value by default?
- When does ownership move?
- Are primitive values copied?
- How are strings passed?
- Will explicit provenance/lifetime syntax ever be needed for cases local
  inference cannot resolve? (Reserved future possibility, not planned for
  KAI 0.1 - see MEMORY_MODEL.md §13.)

## Functions

- Does KAI support function overloading?
- Are default arguments allowed?
- Are named arguments allowed?

## Lexical Grammar

- Will KAI ever support hexadecimal/octal/binary integer literals (e.g. `0xFF`, `0b1010`, `0o755`)?
- Will KAI ever support digit separators (e.g. `1_000`)?
- Will KAI ever support numeric literal suffixes (e.g. `100u64`, `0.5f32`)?
- Will KAI ever support exponent notation for float literals (e.g. `1e10`, `1.2e-5`)?
- What escape sequences beyond the initial set (`\n \r \t \\ \" \0` for strings, `\n \r \t \\ \' \0` for chars) will eventually be supported (e.g. unicode escapes like `\u{1F600}`)?
- Full Unicode identifier support (the KAI 0.1 lexer is ASCII-only; see GRAMMAR.md §2).
- Full Unicode scalar value validation for character literals (the KAI 0.1 lexer accepts only a single ASCII byte or a supported escape; `char` remains defined as a Unicode scalar value at the language level per TYPE_SYSTEM.md).
- Will KAI ever support multiline string literals? (KAI 0.1 treats a raw newline inside `"..."` as invalid lexical input.)

## Tooling

- What information should diagnostics expose?
- Which error codes are stable?
- What should `kai check --json` return?

## Resolved — Draft 0.1

✓ Variables are immutable by default.
✓ `let` declares immutable values.
✓ `mut` declares mutable values.
✓ Local type inference is supported.
✓ Function parameter types are explicit.
✓ Function return types are explicit.
✓ Semicolons are not required.
✓ `{}` delimit blocks.
✓ Integer literals default to i32.
✓ Floating literals default to f64.
✓ Implicit conversions are limited.
✓ 0..n uses an exclusive upper bound.
✓ `str` is a Copy, non-owning, immutable UTF-8 text view (not a reference; bare `str`, not `&str`, for ordinary text parameters/locals). `String` is the future owned, growable, Move UTF-8 buffer. See TYPE_SYSTEM.md §13-17 and MEMORY_MODEL.md §25.
✓ **(KAI LANGUAGE M7A)** Arrays are represented as a real structural semantic type `[T; N]`: element type and compile-time length are both part of the type's own identity (`[i32; 3]` and `[i32; 4]` are distinct types, as are `[i32; 3]` and `[u32; 3]`), inline-owned (no runtime length header), with a backend representation expected to be LLVM's `[N x T]`. Still open: how arrays are PASSED at a function boundary (see the Memory section above) - this resolves the type's own identity/representation only, not an ABI. See TYPE_SYSTEM.md §18 and Type.hpp's own CompoundTypeId documentation.
✓ **(KAI LANGUAGE M7A)** Arrays and slices are distinct, non-interchangeable types: a fixed-size array `[T; N]` owns N elements inline; a slice `[T]` is a separate, non-owning borrowed view. As of M7A, `[T]` did not yet resolve to anything (Type::unresolved()) - see the M10A bullet below for its real semantic Type. See TYPE_SYSTEM.md §18/§20.
✓ **(KAI LANGUAGE M7A, implemented in M7B)** Normal array indexing (`xs[index]`) is CHECKED: `0 <= index < N` for an array of length N, verified at compile time when the index is a compile-time constant, otherwise at runtime; a dynamic out-of-bounds access (including any negative signed index) terminates the program immediately via a non-recoverable trap - this is NOT the language `panic` mechanism, introduces no unwinding/recovery, and its exact OS signal/exit code is not a stable language guarantee. The element address/load/store must never occur before the bounds check succeeds - normal indexing never silently lowers to an unchecked GEP. A future explicitly-unsafe unchecked-indexing operation may be designed separately without changing this normal-indexing contract. See TYPE_SYSTEM.md §18.
✓ **(KAI LANGUAGE M7B)** A fixed-size array is real, native, executable code: literal creation, checked indexed reads/writes (a `mut` binding only), and integration with an M6 `for`-range loop. Arrays as a function parameter or return type, and whole-array assignment/copy, remained unexecutable in M7B (deliberate backend guards) pending the semantic decisions M8A then resolved below, and the physical ABI M8B then implemented.
✓ **(KAI LANGUAGE M8A)** Fixed-size arrays are KAI value types: `let b = a` and `mut a = ...; a = b` (exact same structural array type, `a` mutable) are semantically valid value copies - there is no aliasing, no implicit sharing, no copy-on-write, and `a` and `b` are never a special "shared storage" relationship even conceptually. `[T; N]` is Copy-like exactly when `T` is - no `Copy` trait or ownership system was introduced to say so; this is a plain, documented language rule that a future non-Copy element type may need to refine, not a general mechanism. Self-assignment (`a = a`) is ordinarily valid, with no special-cased language error. Whole-array transfer requires the EXACT structural type (`[i32;3]` to `[i32;3]` only - never `[i32;3]` to `[i32;4]` or `[u32;3]`, and never an implicit element-by-element conversion); an array LITERAL may still use ordinary contextual literal typing (`let xs: [u32; 3] = [1, 2, 3]`) - that is unrelated to, and does not generalize, cross-type array value conversion. Function parameters and return values of a fixed-size array type are semantically BY VALUE - a parameter's own value is independent of the caller's binding (no aliasing), a returned array value has no lifetime relationship to callee locals, and parameters remain immutable under KAI's existing (unchanged) parameter rules - no new mutable-parameter syntax. This is a LANGUAGE-semantics resolution only: the low-level physical ABI was left as a separate, then-unresolved M8B implementation decision with no bearing on these observable semantics. `str` array elements continue following M7B's existing `str` Copy/view contract unchanged - this is not reinterpreted as owned-`String` semantics. No array decay to `[T]`/a pointer/a reference exists or is planned; slices remain a distinct, still-entirely-separate future feature. See TYPE_SYSTEM.md §18/§19 and COMPILER_ARCHITECTURE.md's own M8A note.
✓ **(KAI LANGUAGE M8B)** All of the M8A semantics above are now real, native, executable code, and the low-level physical ABI question is resolved: KAI's initial (and current) strategy is a DIRECT LLVM aggregate argument/return - `[T; N]` lowers straight to `[N x T]` as a `FunctionType` parameter/return type, with no `sret`, no `byval`, no hidden pointer parameter, and no source-level reference/aliasing introduced anywhere. Whole-array initialization/assignment/self-assignment, array-typed function parameters/returns (both an existing array value and an inline array literal as an argument, and both a literal and an existing value as a return expression), `str`-element and zero-length (`[T; 0]`) arrays through all of the above, and nested-array VALUE transport (copying/passing/returning a whole nested array, as opposed to indexing more than one level into one, which remained unimplemented as of M8B) are all executable. This is still an internal backend implementation detail, not a language-level guarantee: KAI does not promise a stable, external, C-compatible ABI for arrays, and a different physical strategy could replace this one without changing any of M8A's observable source semantics. See COMPILER_ARCHITECTURE.md's own M8A/M8B note.
✓ **(KAI LANGUAGE M9)** Indexing more than one level into a nested fixed array (`m[0][1]`, and deeper - `a[i][j][k]`) - the one gap M8B's bullet above explicitly left open - is now real, native, executable code, at any nesting depth the type system supports: each level is independently checked (compile-time when its own index is a compile-time constant, otherwise at runtime) strictly before that level's own element address is computed, using the exact same `ArrayIndexOutOfBounds` diagnostic and `llvm.trap` mechanism a single-level index already had - no new "multidimensional bounds" diagnostic was introduced. Mutation through a nested chain (`matrix[i][j] = v`) is decided by the ROOT binding alone, exactly like a single-level `xs[i] = v` - an intermediate array element never introduces its own mutability, and writing through an immutable root or a parameter root remains rejected via the same existing diagnostics. An array-valued intermediate index result (`let row = matrix[1]`) follows the ordinary M8A/M8B value-copy rule - an independent copy, never an alias. No source-level reference/lvalue system was introduced; the existing M8B direct-aggregate ABI is completely unchanged. See TYPE_SYSTEM.md §18's own "Nested Fixed-Array Indexing" subsection and COMPILER_ARCHITECTURE.md's own M9 note.
✓ **(KAI LANGUAGE M10A)** A slice `[T]` is now a real semantic `TypeKind::Slice`, structurally distinct from Array: its identity is its element Type ALONE (no length - `SemanticModel::internSlice()`/`sliceElementType()`, a sibling interning table to Array's own, per the same compound-type architecture M7A established), so `[i32] == [i32]` regardless of runtime length, `[i32] != [u32]`, and `[i32] != [i32; 3]` (never interchangeable with, and never automatically converted to/from, a fixed array). A slice is a NON-OWNING, IMMUTABLE, runtime-length VIEW: `Slice<T> { ptr, len }` conceptually, not source-visible struct syntax; copying a slice (`let b = a`) copies only the view (ptr+len) - the underlying elements are never copied, and there is no refcount/allocation/copy-on-write, deliberately different from `[T; N]`'s own value-copy semantics. M10A introduced NO mutable-slice variant (`[mut T]`/`mut [T]`/`&mut [T]`) - element mutation through a slice remained unsupported pending a future explicit design; a `mut` BINDING containing a slice may still be reassignable as a binding, which is unrelated to element mutability. A slice function parameter receives a copy of the view (pointer+length by value, no ownership transfer, no aliasing guarantee since multiple read-only views may share storage - safe because no mutation exists); slice RETURN values were a DEFERRED lifetime problem as of M10A - KAI has no general borrow checker, so a slice viewing local storage must never be returned, recorded then only as a design invariant, not yet enforced. M10A itself was TYPE-FOUNDATION-ONLY: no LLVM lowering, no array-to-slice conversion, no slice indexing, and no slice literals existed yet - see the M10B bullet below for what M10A left open. See TYPE_SYSTEM.md's own "Slices" section and COMPILER_ARCHITECTURE.md's own M10A note.
✓ **(KAI LANGUAGE M10B)** Immutable slices are now real, native, executable code. Explicit construction only - the compiler builtin `slice(array)`, never an implicit array-to-slice conversion anywhere (including at a call site: `sum(a)` where `sum` expects `[i32]` remains a genuine `TypeMismatch`; the caller must write `sum(slice(a))`) - resolves the array-to-slice-conversion question the M10A bullet above left open: an EXPLICIT BUILTIN, not implicit coercion or dedicated syntax. `len(x) -> u64` resolves the length-operation question similarly - one dedicated builtin, accepting a fixed array (a compile-time constant, never inspecting runtime memory), a slice, or `str` (its existing byte-length contract, unchanged) - never `x.len`/`sizeof(x)`/a spellable `usize`, and `str` is never reinterpreted as `[u8]`. `slice(x)`'s own source restriction - `x` must be a direct identifier naming a local fixed array or a fixed-array parameter, never a literal/call result/index or member expression/existing slice (`InvalidSliceSource` otherwise) - is what makes the M10B lifetime model enforceable with no dedicated lifetime checker: combined with an unconditional rejection of a Slice RETURN type and of any executable array recursively containing a Slice (preventing indirect escape through M8's own array-return machinery), a Slice's backing storage is always guaranteed, by construction, to dominate every use of that Slice within the current function invocation - a real, ENFORCED restriction now, not merely a documented one, though still deliberately narrow (no general lifetime/provenance analysis, no borrow checker). Checked Slice indexed reads use the SAME `llvm.trap`-guarded runtime-bounds model arrays already use (a shared `lowerCheckedIndexBounds()` helper, factored out of the existing array-indexing code with zero behavior change to it), with a dedicated `SliceIndexOutOfBounds` diagnostic for a compile-time-known negative constant index (never reusing `ArrayIndexOutOfBounds`, whose wording would be misleading for a runtime-length type) and a dedicated `AssignmentThroughImmutableSlice` diagnostic for any indexed write (never `AssignmentToImmutableBinding`, which would misreport this when the slice BINDING itself is `mut`). Slice indexed writes remain unconditionally rejected - element mutation through a slice is still entirely unsupported. See TYPE_SYSTEM.md's own "Slices" section and COMPILER_ARCHITECTURE.md's own M10B note.

✓ **(KAI LANGUAGE M11A)** "Can a Slice be returned?" now has a real, if
deliberately narrow, answer at the SEMANTIC layer: a restricted
provenance analysis - `External` (backed by storage outliving the
current invocation: a Slice parameter, or any plain copy/rebinding of
one), `Local` (backed by storage owned by the current invocation:
`slice(...)` of a local array OR of a fixed-array parameter, since M8's
fixed-array parameters are themselves passed by value into
callee-owned storage - never treated like a Slice parameter), or
`Unknown` (anything else, including the result of any other function
call - no interprocedural inference) - is tracked per binding,
flow-sensitively, across straight-line reassignment, if/else branch
merging (`External+External -> External`, `Local+Local -> Local`,
anything mixed or already-`Unknown` -> `Unknown`; an `if` with no
`else` folds the pre-branch provenance in as the real "no branch taken"
outcome, an exhaustive `if`/`else` does not), and `while`/`for` loops
(any binding reassigned anywhere in the loop body becomes `Unknown`
after the loop, unconditionally - a deliberately coarser, still-sound
approximation, not fixed-point dataflow). Only `External` may be
returned from a function declared to return Slice; `Local`/`Unknown`
are rejected with a dedicated `EscapingLocalSlice` diagnostic, never a
generic `TypeMismatch`. Provenance is a property of VALUES/expressions/
bindings at a point in a function body, never encoded into the Slice
`Type` itself, and it is compiler-internal safety metadata with no
public query surface (`kai inspect`/`kai refs`/... never expose it).
At M11A itself, this was SEMANTIC ANALYSIS ONLY - code generation still
rejected every Slice return unconditionally regardless of how safely its
provenance was proven. See TYPE_SYSTEM.md's own "Function returns"
subsection and COMPILER_ARCHITECTURE.md's own M11A note.

✓ **(KAI LANGUAGE M11B)** Whether an `External` Slice return could be made
actually EXECUTABLE - left open by the M11A bullet above - is now
resolved: **it executes natively.** The backend's own return-type guard
(`isUnsupportedSliceCarryingType()`, replacing M10B's unconditional
`typeContainsSlice(returnType)` check) now allows a bare Slice return
type through - reachable ONLY via a `return` M11A has already proven
`External`, since `Local`/`Unknown` still fail at TypeChecker with
`EscapingLocalSlice` before codegen ever runs. `fn identity(xs: [i32]) ->
[i32] { return xs }` now compiles AND RUNS, transporting only the
`{ptr, i64}` view (never a copy of the viewed elements) back to a caller
whose own backing storage the view was pointing at all along - no
dangling pointer is created, since the returned `ptr` was never rebased
away from storage the CALLER (not the returning function) owns. M11B
duplicates none of M11A's provenance analysis - it only relaxes which
TYPE SHAPES codegen accepts, never re-deciding which specific returns are
safe. An executable array recursively containing a Slice remains
unconditionally rejected regardless of provenance (M11B does not
generalize provenance tracking to aggregates), and re-forwarding an
`Unknown`-provenance Slice-returning call result remains rejected too
(no interprocedural inference was added) - see TYPE_SYSTEM.md's own
"Function returns" subsection and COMPILER_ARCHITECTURE.md's own M11B
note.

## Open — beyond M11B (slices)

The following remain deliberately OPEN (see TYPE_SYSTEM.md's own "Slices"
section):

- Interprocedural provenance inference: M11A/M11B treat the result of ANY
  function call as `Unknown`, even a callee that only ever safely
  forwards its own parameter - whether and how a future milestone infers
  a callee's own return provenance (from its signature, or from analyzing
  its body) remains undesigned.
- General, composite/aggregate provenance: M11A's provenance tracking
  covers bare Slice-typed bindings/expressions only: an executable array
  recursively containing a Slice remains unconditionally rejected at the
  backend regardless of provenance (`isUnsupportedSliceCarryingType()`,
  M11B's renamed successor to M10B's own `typeContainsSlice` guard, still
  rejects it), and neither M11A nor M11B generalizes provenance tracking
  to Slice-containing aggregates.
- A general, sound BORROW CHECKER or reference/lifetime system - M11A/M11B
  are explicitly a narrow, special-cased analysis plus its backend
  enablement (single-level bindings, a fixed small set of expression
  shapes, no lifetime syntax), not a step toward general borrow checking;
  MEMORY_MODEL.md's own "no borrow checker planned for KAI 0.1" stance is
  unaffected.
- Future mutable-slice syntax/semantics (`[mut T]`, `&mut [T]`, or otherwise)
  - M10B/M11A/M11B deliberately ship immutable-slice-only, with no writable
  slice indexing and no mutable-view design.
- Sub-slicing (`s[1..3]` or similar) and slice-of-slice - both explicitly out
  of scope, with no syntax or semantics proposed yet.