# KAI Examples

Status of every tracked `examples/*.kai` file against the current
reference compiler (`kaicc`), verified directly by compiling and running
each one - not by reading the source and guessing. "Compiles" means
`kaicc <file> -o <output>` exits `0`; "runs" means the resulting
executable then exits `0`.

## Verified executable examples

These compile and run today, and their stdout is asserted exactly by
`scripts/build-release-linux-x86_64.sh` before they are allowed into a
release artifact (see that script and `dist/kai-linux-x86_64/examples/`
after a release build).

| File | What it demonstrates | Exact stdout |
|---|---|---|
| `hello.kai` | The minimal program: a string literal and `print` | `Hello from KAI` |
| `functions.kai` | Functions, parameters, calls, and a `str` parameter | `Hello`, `KAI`, `42`, `84` |
| `conditions.kai` | `if`/`else if`/`else` | `adult`, `positive`, `negative`, `zero` |
| `variables.kai` | `let`, `mut`, reassignment, `f32`/`i32` locals | `KAI`, `0.1`, `2026`, `1` |
| `loops.kai` | `while`, then a `for` loop over an integer literal range (KAI LANGUAGE M6, post-alpha.2) | `0`,`1`,`2`,`3`,`4` (from `while`), then `0`,`1`,`2`,`3`,`4` again (from `for n in 0..5`) |
| `fibonacci.kai` | Recursion + a `for` loop over an integer literal range (KAI LANGUAGE M6, post-alpha.2), printing the first 10 Fibonacci numbers | `0`,`1`,`1`,`2`,`3`,`5`,`8`,`13`,`21`,`34` |
| `arrays.kai` | Local fixed-size arrays (KAI LANGUAGE M7B, post-alpha.2): a literal `[i32; 4]`, checked indexed reads, an M6 `for`-range loop indexing it, and a mutable array with a checked indexed write inside another `for`-range loop | `10`, `40`, `10`,`20`,`30`,`40`, `1`,`1`,`1` |

## Diagnostic example (intentionally invalid)

| File | Purpose |
|---|---|
| `errors.kai` | Demonstrates real compiler diagnostics on purpose - an unknown identifier, two type mismatches, and an assignment to an immutable binding. **This file is expected to fail to compile.** A clean compile of `errors.kai` would indicate a compiler regression, not a fix. |

## Design/future examples (not currently executable)

These files predate the current MVP compiler and describe language
directions that are not implemented yet. They are kept as design sketches,
not deleted, but they do **not** compile with the current `kaicc` and are
**not** included in release artifacts. Each currently fails for one of
these reasons:

- **Arrays as function parameters/returns, and slices, remain
  unsupported.** KAI LANGUAGE M7B (post-alpha.2) makes a LOCAL fixed-size
  array `[T; N]` fully executable - literal creation, checked indexed
  reads/writes, integration with an M6 `for`-range loop - see
  `arrays.kai` above. What remains explicitly out of scope: arrays as a
  function PARAMETER or return type (no array calling-convention/ABI has
  been designed - such a parameter/return still fails with `code
  generation is not yet supported for this parameter's type`, the same
  message a still-unsupported type already produces), whole-array
  assignment/copy (`let b = a` / `a = b` for two array-typed values -
  deliberately kept unsupported pending explicit language-semantics
  review, not merely unimplemented by oversight), and slice syntax
  (`[T]`, still fully deferred at the type level, `Type::unresolved()`).
- **`for` iteration over anything other than a literal integer range is
  not yet supported.** KAI LANGUAGE M6 (post-alpha.2) makes
  `for i in start..end` over integers a real, executable, native loop -
  see `loops.kai`/`fibonacci.kai`/`arrays.kai` above - but KAI 0.1 still
  has no general iterable protocol/arrays-as-iterables/iterators: `for x
  in someArray` fails as a **semantic** error (`unsupported for-loop
  iterable`), before code generation is ever reached.

| File | Why it doesn't compile today |
|---|---|
| `calculator.kai` | Compares two `str` values with `==` (`op == "+"`) - string equality is not implemented; this is a **semantic** error (`invalid binary operands`), not a codegen limitation. |
| `mini_program.kai` | Uses a slice-typed parameter (`average(values: [f64])`), `for` iteration over an array, and array member access (`.len`) - fails with a **semantic** `unsupported for-loop iterable` error (KAI LANGUAGE M6), before compilation ever reaches the codegen stage where the slice-typed parameter would also be rejected. |
| `results.kai` | Sketches `Result<(), IOError>` and the `?` operator against an undefined `write_file()`/`IOError` - a design fragment with no `main`, not a runnable program; fails with `unknown identifier`. |

See the root [`README.md`](../README.md)'s "Current limitations" section
and [`ROADMAP.md`](../ROADMAP.md) for the actual implementation status of
slices, array function parameters/returns, and general `for` iteration.

### `arrays.kai` history

An earlier release audit found the ORIGINAL `arrays.kai` declared **two**
`fn main()` functions - a genuine authoring mistake, unrelated to array
support, that predates the compiler existing to catch it - which was
fixed by merging both bodies into one `main()`. That version still relied
on general array iteration (`for value in values`) and a slice-typed
parameter, both still unsupported, so the file stayed in this "design/
future" section under "why it doesn't compile today" for a while even
after that structural fix. KAI LANGUAGE M7B (post-alpha.2) then rewrote
`arrays.kai` entirely around the now-genuinely-supported subset (literal
creation, indexed reads/writes, `for`-range integration - see "Verified
executable examples" above) - the general-iteration/slice-parameter
content was removed rather than fixed, since neither is in scope for
M7B.

## Adding a new example

If you add a `.kai` file to this directory, verify it end-to-end
(`kaicc <file> -o out && ./out`) and add it to the appropriate table above.
Only add a file to "Verified executable examples" - and only then also
consider adding it to `scripts/build-release-linux-x86_64.sh`'s curated
release list - once its exact stdout is stable and asserted.
