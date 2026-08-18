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