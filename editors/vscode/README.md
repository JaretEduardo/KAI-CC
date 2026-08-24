# KAI Language Support for VS Code

Editor support for [KAI](../../LANGUAGE_DESIGN.md), the experimental
AI-native, statically-typed, compiled systems programming language. This
extension lives inside the KAI-CC repository so the extension and compiler
version together.

## Current functionality (Milestone 1)

- **Syntax highlighting** for `.kai` files: keywords (`fn`, `let`, `mut`,
  `return`, `if`, `else`, `while`, `for`, `in`, `struct`, `enum`, `use`,
  `pub`, `as`), booleans (`true`/`false`), primitive types (`i8`-`i64`,
  `u8`-`u64`, `f32`, `f64`, `bool`, `char`, `str`), string/char literals
  with their supported escape sequences, integer/float literals, `//` line
  comments, function names, and current operators/punctuation.
- **Language configuration**: line comments, bracket/auto-closing/
  surrounding pairs, and basic indentation on `{`/`}`.
- **Snippets**: `fn`, `main`, `if`, `ifelse`, `while`, `for`.

All of the above is declarative (TextMate grammar + JSON configuration) -
there is no bundled extension code and no dependency on `kaicc` being
built or runnable.

## Current limitations

- No compiler integration yet: **`KAI: Build Current File`** and
  **`KAI: Run Current File`** are not implemented. They depend on a
  bundled `kaicc` binary, which arrives once the LLVM backend produces
  real executables.
- No Language Server Protocol support: no completion, hover,
  go-to-definition, rename, or semantic tokens. This is post-MVP work,
  planned once the compiler exposes a queryable semantic model (see
  `COMPILER_ARCHITECTURE.md` §9).
- The syntax grammar reflects only the currently implemented KAI 0.1
  lexical grammar (`GRAMMAR.md`) and the shipped `examples/*.kai` files -
  it intentionally does not highlight syntax that is only a roadmap idea
  (generics, traits, closures, etc. are not part of KAI 0.1).

## Platform support

- **Now (verified)**: Linux x64 - the only platform this development
  environment can currently build and test against.
- **Planned, not yet verified**: Windows x64, macOS. No claim of support
  for these platforms is made until they have been tested.

Because this milestone has no native binary dependency, the extension
itself is platform-independent. Platform specificity will only matter once
a bundled `kaicc` binary is added (see below).

## Local development

From `editors/vscode/`:

```sh
code --extensionDevelopmentPath="$(pwd)" $(pwd)/../../examples/hello.kai
```

This opens a VS Code Extension Development Host window with the extension
loaded, showing highlighted KAI syntax without installing anything.

## Future: bundled compiler integration

Not implemented in this milestone. The intended shape, once the LLVM
backend produces a working `kaicc`:

- `editors/vscode/bin/<platform>/kaicc` (or a platform-specific VSIX)
  bundling the native compiler binary.
- `src/compiler.ts` - resolves the bundled (or system) `kaicc` path.
- `src/commands.ts` - registers `KAI: Build Current File` and
  `KAI: Run Current File`.
- `src/diagnostics.ts` - maps `kaicc --json` diagnostics onto VS Code's
  `Diagnostic` API.
- `src/extension.ts` - thin activation entry point wiring the above
  together.

Packaging will follow VS Code's platform-specific VSIX mechanism so each
package only bundles the `kaicc` binary for its target platform.
