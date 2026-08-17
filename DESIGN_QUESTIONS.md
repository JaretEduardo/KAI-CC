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