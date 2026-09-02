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
| `arrays.kai` | Fixed-size arrays: a literal `[i32; 4]`, checked indexed reads, a checked indexed write inside a `for`-range loop (KAI LANGUAGE M7B, post-alpha.2), an EXISTING array value passed by value into/out of a `sum(xs: [i32; 4]) -> i32` function (KAI LANGUAGE M8B, post-alpha.2), and a nested `[[i32; 2]; 2]` with a two-level checked read and a two-level checked write (KAI LANGUAGE M9, post-alpha.2) | `10`, `40`, `100`, `10`,`20`,`30`,`40`, `1`,`1`,`1`, `2`, `99` |
| `slices.kai` | Immutable slice VALUES (KAI LANGUAGE M10B, post-alpha.2): explicit `slice(values)` construction over a fixed array (no implicit array-to-slice conversion), `len(s)`, and a `sum(xs: [i32]) -> i32` Slice function parameter iterating via `for i in 0..len(xs)` | `4`, `100` |

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

- **Fixed-size arrays and immutable slices are now both fully executable,
  including across function boundaries; only slice RETURNS and Slice-
  containing aggregates remain deliberately unsupported.** KAI LANGUAGE
  M7B (post-alpha.2) made a LOCAL fixed-size array `[T; N]` fully
  executable - literal creation, checked indexed reads/writes,
  integration with an M6 `for`-range loop - see `arrays.kai` above. KAI
  LANGUAGE M8A (post-alpha.2) then resolved the remaining LANGUAGE
  semantics: fixed arrays are value types (`let b = a` / whole-array
  `a = b` are ordinary value copies, no aliasing), and a function
  parameter/return of array type is likewise semantically by value with
  exact structural type matching. KAI LANGUAGE M8B (post-alpha.2) then
  implemented all of that as real native code: whole-array
  initialization/assignment/self-assignment, array function
  parameters/returns (lowered as a direct LLVM aggregate `[N x T]`
  argument/result - never `sret`/`byval`/a hidden pointer, and never a
  promised stable external C ABI), and array-literal values used directly
  as call arguments/return expressions - see `arrays.kai`'s `sum()`
  function above. KAI LANGUAGE M9 (post-alpha.2) then completed general
  nested indexing (`matrix[i][j]`, and deeper - `a[i][j][k]` - at any
  fixed-array nesting depth): each level is independently checked before
  its own element address is ever computed, mutation through a nested
  chain is allowed exactly when the ROOT binding is a mutable local (an
  intermediate array element never introduces its own mutability), and an
  array-valued intermediate index result (`let row = matrix[1]`) is an
  ordinary independent copy, not an alias - see `arrays.kai`'s `matrix`
  demonstration above. KAI LANGUAGE M10A (post-alpha.2) then gave slices
  `[T]` a real semantic `TypeKind::Slice` type (structurally distinct from
  Array - no length in the type). KAI LANGUAGE M10B (post-alpha.2) then
  made slices genuinely executable: explicit `slice(array)` construction
  (never an implicit array-to-slice conversion - `sum(a)` where `sum`
  expects `[i32]` remains a real TypeMismatch), `len(array | slice | str)`,
  checked Slice reads with the same `llvm.trap`-guarded runtime bounds
  model arrays already use, and a Slice function PARAMETER (copy of the
  view, by value) - see `slices.kai` above. KAI LANGUAGE M11A
  (post-alpha.2) then added a restricted PROVENANCE analysis at the
  SEMANTIC layer only (`External`/`Local`/`Unknown`, never part of the
  Slice Type itself): TypeChecker now accepts a narrow, provably-sound
  class of Slice returns (e.g. `fn identity(xs: [i32]) -> [i32] { return xs }`,
  relaying a parameter's own slice straight back out) instead of
  rejecting every Slice return outright, with anything else rejected via
  a dedicated `EscapingLocalSlice` diagnostic - but code generation is
  completely unchanged, so even a proven-safe return like `identity`
  above still fails cleanly at the backend today. What remains explicitly
  out of scope, by deliberate lifetime-safety design rather than a
  lowering gap: an EXECUTABLE Slice return of any kind (even a
  provenance-safe one), interprocedural provenance inference, any
  executable array that recursively contains a Slice (preventing indirect
  escape through M8's own array-return machinery), slice-of-slice,
  sub-slicing, mutable slice elements/syntax, and `for x in someArray`
  iteration.
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
| `mini_program.kai` | Uses a slice-typed parameter (`average(values: [f64])`), `for` iteration over an array, and array member access (`.len`) - fails with TWO **semantic** errors: `unsupported for-loop iterable` (KAI LANGUAGE M6) and, as of KAI LANGUAGE M10A giving `[f64]` a real Slice Type, a genuine `type mismatch: expected [f64], got [f64; 4]` at its one call site (`average(scores)` where `scores: [f64; 4]` - a fixed array is never interchangeable with a slice of the same element type, TYPE_SYSTEM.md's own "Array vs. Slice" rule). Both are frontend diagnostics; codegen is never reached either way. |
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
