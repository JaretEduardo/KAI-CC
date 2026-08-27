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

- **Now (bundled compiler, verified)**: Linux x64 and Windows x64, each as its
  own separate `.vsix` (one VSIX never bundles both platforms' compilers - see
  "Packaging" below). On any other `process.platform`/`process.arch`
  (macOS, ARM64, ...), both commands show a clear error naming the
  supported platforms rather than guessing.
- **Compiling KAI programs still requires a working host C toolchain** (a
  `clang`/`gcc`-compatible driver) on `PATH`, on either platform - the
  bundled `kaicc`/`kaicc.exe` itself needs no separate LLVM/MSYS2
  installation to run, but the final native link step shells out to a host
  compiler driver exactly like the portable CLI releases do (see the root
  `README.md`'s "Portable Linux release"/"Portable Windows release"
  sections). If it's missing, the build fails with a clear message rather
  than an obscure linker error.
- **Development override**: set the `kai.compilerPath` setting to point at
  any other `kaicc`/`kaicc.exe` binary (e.g. a local build for testing) -
  this is an explicit opt-in, never a silent `PATH` search, so packaged
  behavior stays deterministic.
- Syntax highlighting/snippets/language configuration remain fully
  platform-independent (declarative only).

No macOS native compiler support is claimed. No Language Server
Protocol support (completion, hover, go-to-definition, rename, semantic
tokens) is implemented - planned post-MVP once the compiler exposes a
queryable semantic model (see `COMPILER_ARCHITECTURE.md` §9).

## Bundled compiler

Each platform-specific `.vsix` bundles ONE platform's native compiler/runtime:

```
bin/kaicc                       (Linux x64 VSIX)
lib/kai/libkai_runtime.a
lib/kai/libz3.so.4              (if the underlying LLVM build depends on it)
```

```
bin/kaicc.exe                   (Windows x64 VSIX)
bin/libgcc_s_seh-1.dll
bin/libstdc++-6.dll
bin/libwinpthread-1.dll
bin/zlib1.dll
bin/libzstd.dll
lib/kai/libkai_runtime.a
```

`bin/` and `lib/kai/` are kept as SIBLINGS deliberately: `kaicc`'s own
runtime-library lookup (`NativeLinker::findDefaultRuntimeLibrary()`, see
`compiler/include/kai/codegen/NativeLinker.hpp`) looks for
`<kaicc's own directory>/../lib/kai/libkai_runtime.a` relative to
wherever it is actually running - this layout is exactly what makes that
lookup succeed from inside an installed extension directory, with zero
extra configuration. The Windows DLLs sit directly beside `kaicc.exe` in
`bin/` for the same reason: Windows always searches an executable's own
directory for its DLL dependencies first, so no `PATH` changes are needed
there either.

These files are **build artifacts, not source** - they are `.gitignore`'d
and must be staged locally before packaging (see below). The exact staged
set always comes from the corresponding portable release root
(`dist/kai-linux-x86_64/` or `dist/kai-windows-x86_64/`), never
hand-picked or reconstructed independently - see `scripts/stage-compiler.mjs`.

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

**Ordinary local development** (stages your own host's `build/` tree - Linux
only, since that's what a plain local `cmake --build build` produces):

```sh
cmake -B build -S .   # from the repository root
cmake --build build
cd editors/vscode
npm install
npm run compile        # TypeScript -> out/*.js
npm run stage-compiler # copies build/bin/kaicc + build/lib/kai/libkai_runtime.a
                        # into bin/ and lib/kai/ here
npm run package         # tsc + stage-compiler + vsce package -> a .vsix file
```

**A real, portable, platform-specific release VSIX** is built from the
corresponding portable release root instead - never from a raw local build
tree or a hand-picked set of binaries (see the root `README.md`'s
"Portable Linux release"/"Portable Windows release" sections for how those
release roots themselves are produced):

```sh
# Linux x64 VSIX, from dist/kai-linux-x86_64/:
KAI_RELEASE_ROOT=/path/to/dist/kai-linux-x86_64 npm run package:linux-x64

# Windows x64 VSIX, from dist/kai-windows-x86_64/ (run inside an MSYS2
# UCRT64 shell, or with KAI_RELEASE_ROOT pointing at a copy of it):
KAI_RELEASE_ROOT=/path/to/dist/kai-windows-x86_64 npm run package:win32-x64
```

Each produces its own separate `.vsix` (`vsce package --target linux-x64` /
`--target win32-x64`) - a single VSIX never bundles both platforms' ~20 MB
compilers; a user's VS Code selects the right one automatically based on
their own platform. `KAI_RELEASE_ROOT` is the only thing that changes
between the two commands; the staging script (`scripts/stage-compiler.mjs`)
detects which target a given release root actually contains and rejects a
mismatched/incomplete one rather than guessing.

Install a produced VSIX locally with:

```sh
code --install-extension kai-language-<version>-<target>.vsix --force
```

Not implemented: marketplace publication. `.vsix` files, the staged
`bin/`/`lib/` artifacts, and `node_modules/` are never committed to git
(see `.gitignore`) but ARE included in the packaged `.vsix` (see
`.vscodeignore`, which explicitly re-includes them despite being
git-ignored).
