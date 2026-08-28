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
- How are arrays passed?
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
✓ **(KAI LANGUAGE M7A)** Arrays and slices are distinct, non-interchangeable types: a fixed-size array `[T; N]` owns N elements inline; a slice `[T]` is a separate, still-future, non-owning borrowed view. `[T]` does not resolve to Array and has no semantic Type representation yet. See TYPE_SYSTEM.md §18/§20.
✓ **(KAI LANGUAGE M7A)** Normal array indexing (`xs[index]`) is CHECKED: `0 <= index < N` for an array of length N, verified at compile time when the index is a compile-time constant, otherwise at runtime; a dynamic out-of-bounds access (including any negative signed index) terminates the program immediately via a non-recoverable trap - this is NOT the language `panic` mechanism, introduces no unwinding/recovery, and its exact OS signal/exit code is not a stable language guarantee. The element address/load/store must never occur before the bounds check succeeds - normal indexing never silently lowers to an unchecked GEP. A future explicitly-unsafe unchecked-indexing operation may be designed separately without changing this normal-indexing contract. Documented in M7A; implemented in M7B. See TYPE_SYSTEM.md §18.