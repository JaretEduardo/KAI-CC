# KAI Language Support for VS Code

Editor support for [KAI](../../LANGUAGE_DESIGN.md), the experimental
AI-native, statically-typed, compiled systems programming language. This
extension lives inside the KAI-CC repository so the extension and compiler
version together.

## Current functionality (Milestone 4)

- **A proper extension icon** - shown in the Extensions view, search
  results, and the Marketplace listing (see "Branding" below).
- **KAI file icons** - `.kai` files get a distinct icon via a small
  bundled icon theme, once selected (see "File icons" below - this is a
  genuine VS Code limitation, not a bug: only one file icon theme can be
  active at a time).
- **Syntax highlighting** for `.kai` files: keywords (`fn`, `let`, `mut`,
  `return`, `if`, `else`, `while`, `for`, `in`, `struct`, `enum`, `use`,
  `pub`, `as`), booleans (`true`/`false`), primitive types (`i8`-`i64`,
  `u8`-`u64`, `f32`, `f64`, `bool`, `char`, `str`), string/char literals
  with their supported escape sequences, integer/float literals, `//` line
  comments, function names, and current operators/punctuation.
- **Language configuration**: line comments, bracket/auto-closing/
  surrounding pairs, and basic indentation on `{`/`}`.
- **Snippets**: `fn`, `main`, `if`, `ifelse`, `while`, `for`, `let`, `mut`.
- **Basic IntelliSense**: as you type, suggests KAI keywords, primitive
  types, and the `print` builtin - see below for exactly what this does
  and does not cover.
- **`KAI: Build Current File`** - compiles the active `.kai` file with the
  real `kaicc` compiler into a native executable.
- **`KAI: Run Current File`** - builds (same as above), then runs the
  produced executable, showing its output.

Syntax highlighting/language configuration/snippets are purely declarative
(TextMate grammar + JSON configuration). Basic IntelliSense, the icon
theme command/prompt, and the two compiler commands are backed by real
TypeScript extension code (`src/`); the commands bundle and invoke the
actual native `kaicc` compiler built by this repository's CMake build -
see "Bundled compiler" below.

## Branding

The extension ships its own icon (`assets/icon.png`, generated from
`assets/icon.svg`) - a simple, original geometric "K" monogram, wired
through `package.json`'s standard `"icon"` field. This is what appears in
the Extensions view, extension search results, and (once published) the
Marketplace listing.

## File icons

`.kai` files get a distinct icon (the same "K" monogram as the extension's
own icon, for consistent branding) via a small bundled **KAI File Icons**
icon theme (`fileicons/kai-icon-theme.json`, contributed through
`contributes.iconThemes`). The theme is deliberately narrow: it defines
the KAI file icon plus a plain generic fallback for every other file/
folder, rather than trying to be a full-featured icon theme.

**Important VS Code limitation**: only one file icon theme can be active
in a workbench at a time. If you already use another icon theme (e.g.
Material Icon Theme, Seti), installing this extension does **not**
automatically show KAI file icons - you first need to switch to
**KAI File Icons** yourself. Two ways to do that:

- Run **KAI: Use KAI File Icons** from the Command Palette - this
  switches your active icon theme directly, with no picker needed.
- Or use VS Code's own `Preferences: File Icon Theme` picker and choose
  "KAI File Icons" from the list.

The extension never switches this for you automatically, and never
prompts you to - KAI File Icons is a narrow theme (see above), and
switching to it replaces whatever richer icon theme you already have,
falling every non-KAI file/folder back to a plain generic icon. That is a
global, disruptive change worth making deliberately, not something to
suggest on activation.

## Basic IntelliSense

As you type in a `.kai` file, VS Code will suggest:

- **Keywords** - every current KAI keyword (`fn`, `let`, `mut`, `return`,
  `if`, `else`, `while`, `for`, `in`, `struct`, `enum`, `use`, `pub`, `as`,
  `true`, `false`), each with a short description.
- **Primitive types** - `i8`-`i64`, `u8`-`u64`, `f32`, `f64`, `bool`,
  `char`. Right after typing a type-annotation colon or `->` (e.g.
  `let age: |` or `fn f() -> |`), these are ranked above keywords so the
  most useful suggestions appear first.
- **`print`** - inserts `print(value)` with `value` ready to fill in. This
  is the one builtin with real native support today (see the M6/M7
  milestones); `panic`/`assert` exist in the language's reserved-name
  list but have no working implementation yet, so they are intentionally
  left out of autocomplete rather than suggesting something that would
  fail if actually run.

**What this deliberately does NOT do** (this is basic, keyword-level
IntelliSense, not a Language Server):

- No suggestions for your own variables or functions (e.g. a variable you
  named `counter` will not be suggested when you type `cou`).
- No hover information, go-to-definition, rename, or find-references.
- No signature help derived from a function's actual parameters.
- No semantic error checking/diagnostics as you type.

That level of tooling requires the compiler to expose a queryable semantic
model to the editor, which is planned post-MVP (see
`COMPILER_ARCHITECTURE.md` §9) - this milestone intentionally stays inside
the ordinary VS Code extension API (no language server, no background
compiler process).

## Commands

| Command | Id | Behavior |
| --- | --- | --- |
| KAI: Build Current File | `kai.buildCurrentFile` | Saves the active `.kai` file if dirty, compiles it with `kaicc <file> -o <output>`, reports success/failure. |
| KAI: Run Current File | `kai.runCurrentFile` | Runs Build Current File, then executes the produced native binary and shows its output/exit code. |
| KAI: Use KAI File Icons | `kai.useKaiFileIcons` | Switches the active workbench icon theme to KAI File Icons directly (no picker). |

Both commands require an active editor showing a saved (filesystem-backed)
`.kai` file; they do nothing (with a clear error message) for an untitled
buffer, a non-KAI file, or when no editor is active.

Build output convention: for `/project/src/hello.kai`, the produced
executable is `/project/src/hello` (same directory, `.kai` extension
stripped). Re-running Build/Run overwrites that same path - there is no
build directory, manifest, or project system yet.

All compiler stdout/stderr, and the executed program's stdout/stderr, are
shown in a dedicated **"KAI"** Output Channel. On failure that channel is
revealed automatically; on success a brief VS Code notification appears
instead of forcing the channel open.

## Compiler diagnostics

KAI-CC's current CLI output (parse/semantic/backend errors) is plain text,
not a structured/machine-readable format yet. This milestone shows that
text as-is in the "KAI" Output Channel - there is **no** Problems-panel
integration (no squiggles, no `vscode.Diagnostic` objects) yet. That is
planned once the compiler commits to a stable diagnostic format (see
`COMPILER_ARCHITECTURE.md` §10).

## Platform support

- **Now (bundled compiler, verified)**: Linux x64 only. The bundled
  `kaicc`/runtime are built for this platform; on any other
  `process.platform`/`process.arch`, both commands show: *"Bundled KAI
  compiler is currently available only for Linux x64."*
- **Development override**: set the `kai.compilerPath` setting to point at
  any other `kaicc` binary (e.g. a local build for testing) - this is an
  explicit opt-in, never a silent `PATH` search, so packaged behavior stays
  deterministic.
- Syntax highlighting/snippets/language configuration remain fully
  platform-independent (declarative only).

No Windows/macOS native compiler support is claimed. No Language Server
Protocol support (completion, hover, go-to-definition, rename, semantic
tokens) is implemented - planned post-MVP once the compiler exposes a
queryable semantic model (see `COMPILER_ARCHITECTURE.md` §9).

## Bundled compiler

The packaged extension bundles the native compiler/runtime at:

```
bin/kaicc
lib/kai/libkai_runtime.a
```

kept as SIBLINGS deliberately: `kaicc`'s own runtime-library lookup
(`NativeLinker::findDefaultRuntimeLibrary()`, see
`compiler/include/kai/codegen/NativeLinker.hpp`) looks for
`<kaicc's own directory>/../lib/kai/libkai_runtime.a` relative to
wherever it is actually running - this layout is exactly what makes that
lookup succeed from inside an installed extension directory, with zero
extra configuration.

These two files are **build artifacts, not source** - they are
`.gitignore`'d and must be staged locally before packaging (see below).

## Local development

Extension Development Host (no bundled compiler needed just to see syntax
highlighting):

```sh
cd editors/vscode
npm install
npm run compile
code --extensionDevelopmentPath="$(pwd)" ../../examples/hello.kai
```

To also exercise Build/Run Current File in that dev host, either stage the
real compiler first (see "Packaging" below) or set `kai.compilerPath` in
your VS Code settings to point directly at a locally built `build/bin/kaicc`.

Run the extension's own unit tests (plain Node, no VS Code needed - pure
path/policy logic in `src/paths.ts`, process-spawning in `src/process.ts`,
IntelliSense metadata/context logic in `src/completionData.ts`, static
consistency checks over `package.json`'s icon/iconThemes wiring and
`fileicons/kai-icon-theme.json`, the `snippets/kai.json` content, and the
staging script):

```sh
npm test
```

## Packaging

From the repository root, build the compiler first:

```sh
cmake -B build -S .
cmake --build build
```

Then, from `editors/vscode/`:

```sh
npm install
npm run compile        # TypeScript -> out/*.js
npm run stage-compiler # copies build/bin/kaicc + build/lib/kai/libkai_runtime.a
                        # into bin/ and lib/kai/ here
npm run package         # tsc + stage-compiler + vsce package -> a .vsix file
```

(`npm run package` runs all three steps together.) Install the result
locally with:

```sh
code --install-extension kai-language-<version>.vsix --force
```

Not implemented: marketplace publication. `.vsix` files, the staged
`bin/`/`lib/` artifacts, and `node_modules/` are never committed to git
(see `.gitignore`) but ARE included in the packaged `.vsix` (see
`.vscodeignore`, which explicitly re-includes them despite being
git-ignored).
