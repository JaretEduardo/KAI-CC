# KAI Design Questions

## Syntax

- Should final function expressions imply return?
- Should ranges support `..=`?
- Should compound operators such as `+=` exist?
- Should blocks be expressions?

## Types

- What is the default integer type?
- What exactly is `str`?
- Are strings immutable?
- How are arrays represented?
- Do we distinguish arrays from slices?

## Memory

- Are arguments passed by value by default?
- When does ownership move?
- Are primitive values copied?
- How are strings passed?
- How are arrays passed?

## Functions

- Does KAI support function overloading?
- Are default arguments allowed?
- Are named arguments allowed?

## Lexical Grammar

- What is the exact character grammar for integer literals (decimal only, or hex/octal/binary prefixes)?
- What is the exact character grammar for float literals (exponents, leading/trailing dot)?
- Will numeric literals support suffixes (e.g. `100u64`, `0.5f32`)?
- Will numeric literals support digit separators (e.g. `1_000`)?
- What escape sequences do string literals support (`\n`, `\"`, `\\`, unicode escapes)?
- What escape sequences do character literals support?
- What is a `char_literal` exactly — a single byte, a Unicode scalar value, or a grapheme? How is this validated?

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