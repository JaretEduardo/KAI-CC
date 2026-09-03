# Contributing to KAI-CC

KAI-CC is an early, experimental, largely solo-maintained reference compiler for
an still-unstable language. Contributions are welcome, but scope discipline
matters more here than in most projects: KAI 0.1's feature boundary is
deliberately narrow (see [`KAI_0_1_SCOPE.md`](KAI_0_1_SCOPE.md) and "Current
implementation status" in [`COMPILER_ARCHITECTURE.md`](COMPILER_ARCHITECTURE.md)),
and most interesting-looking gaps (structs, generics, a general borrow checker,
mutable slices, ...) are gaps on purpose, not oversights.

## Before you write code

- **Check the design docs first.** Each design document
  (`LANGUAGE_DESIGN.md`, `SYNTAX.md`, `GRAMMAR.md`, `TYPE_SYSTEM.md`,
  `MEMORY_MODEL.md`, `MODULE_SYSTEM.md`, `ERROR_MODEL.md`,
  `STANDARD_LIBRARY.md`, `PROJECT_MODEL.md`, `COMPILER_ARCHITECTURE.md`,
  `KAI_0_1_SCOPE.md`, `ROADMAP.md`) covers one concern and is the source of
  truth for intended syntax/semantics - not folklore, not what a similar
  language does, not what "the compiler does X" claims in prose that hasn't
  been checked against the actual code. `DESIGN_QUESTIONS.md` lists genuinely
  open questions; if your change would answer one, open a design-question
  issue before writing code, not after.
- **Scope discipline: many missing features are intentionally deferred.**
  Slices are immutable and non-owning with a restricted, non-general
  provenance analysis (see TYPE_SYSTEM.md's "Slices" section); arrays are
  fixed-size value types; there is no borrow checker, no
  structs/enums/generics/traits, no general iteration protocol, and no `for`
  loop features beyond a literal integer range. If a PR quietly makes one of
  these "just work" beyond what's documented as implemented, that's a design
  decision that needs its own discussion first, not a drive-by fix.
- **Small, focused changes.** One milestone or one bug per PR, and no
  unrelated cleanup mixed into a focused PR - opportunistic refactors,
  formatting churn, or drive-by renames belong in their own PR so the actual
  change under review stays reviewable.

## Building

```sh
cmake -B build -S . -G Ninja
cmake --build build
```

Requires CMake >= 3.20, a C++23 compiler, and LLVM 22 development packages -
see the root [`README.md`](README.md)'s "Building from source" section for
exact package names on the distribution this project is developed against
(Fedora).

## Running tests

```sh
ctest --test-dir build --output-on-failure
```

Tests use a tiny in-repo `CHECK` macro (`tests/support/check.hpp`), not an
external framework - follow the existing `kai_add_test(...)`/`CHECK`
convention (see any file under `tests/`) rather than introducing a different
test runner or assertion style. **A language or compiler change should come
with a focused test at the right layer**: frontend/semantic tests for
diagnostics and type-checking, codegen tests for LLVM IR shape, and
native-compilation (integration) tests for actual compile-and-run behavior
with exact stdout. **A user-visible language change should also update the
relevant docs and examples** (the design doc it affects, `examples/README.md`
if it changes example status, and the example files themselves) in the same
PR - not as a follow-up.

If you touch the VS Code extension (`editors/vscode/`), it has its own
Node-based test step - see
[`editors/vscode/README.md`](editors/vscode/README.md).

## Architectural rules

`COMPILER_ARCHITECTURE.md` §17 lists hard constraints on how compiler stages
relate to each other (the lexer must not parse, the parser must not generate
code, semantic analysis must stay independent of LLVM, diagnostics must be
structured objects rather than printed directly, LLVM must remain a backend
implementation detail never exposed in KAI syntax, and so on). A change that
needs to cross one of those boundaries is very likely solving the problem at
the wrong layer.

## AI-native benchmark trials

`benchmarks/ai-native/v1/` is a controlled harness for measuring whether
compiler semantic queries help an agent - it is infrastructure, not a
results generator. **Do not run formal benchmark trials casually** while
working on something else; a real trial needs the isolation procedure in
`benchmarks/ai-native/v1/ISOLATION.md` followed exactly, and this project's
public trial counts (`textual: 0, semantic: 0`, stated in the README and
release notes) should stay accurate. If you deliberately run an authorized
trial, record it properly rather than leaving stray trial artifacts in the
repository.

## Branches and commits

This repository's history uses short `<scope>/<slug>` branch names merged via
pull request - e.g.:

```
feat/slice-lifetimes
fix/<short-description>
release/<version>
```

and commit subjects in the form `<scope>: <imperative summary>`, e.g.:

```
lang: implement immutable slice values and checked indexing
docs: fix stale platform status in COMPILER_ARCHITECTURE.md
ci: classify resolved bash by path in fresh-user smoke
build: add portable Windows x86_64 packaging
```

Match this style rather than inventing a new one.

## Where language design questions belong

If you're proposing a language-semantics change rather than fixing a bug in
an already-decided design, that belongs in a design-question issue (see
"Reporting bugs and proposals" below and `DESIGN_QUESTIONS.md`) - not
directly as a PR that changes
`TYPE_SYSTEM.md`/`LANGUAGE_DESIGN.md`/`SYNTAX.md` and the implementation in
the same breath. Nothing in the `*.md` design documents is a final, stable
spec, but changing them is still a design decision, not a documentation
formatting change.

## Reporting bugs and proposals

Open a GitHub issue using the provided templates:

- **Compiler bug** - include the exact `.kai` source that triggers the
  problem, the `kaicc` invocation, what you expected, and what actually
  happened (exact diagnostic text or exit code - see
  [`docs/CLI.md`](docs/CLI.md) for exit code meanings). A minimal
  reproduction is far more useful than a large program.
- **Language/design proposal** - state the problem being solved, not just a
  desired syntax. A reasonable-sounding proposal may still be deferred if it
  doesn't fit KAI 0.1's current scope; that's expected, not a rejection of
  the feedback itself.
