# kaicc Command-Line Reference

`kaicc` is the single command-line entry point for the KAI reference
compiler. There is no separate `kai` wrapper yet - every capability below
is invoked directly through `kaicc`.

Run `kaicc --help` at any time for a short, categorized summary of the
same commands.

## General

```
kaicc --version
kaicc --help
```

## Compile

```
kaicc <file.kai> -o <output>
```

Compiles `<file.kai>` all the way to a native executable at `<output>`.
This requires a working host C toolchain to be available: a
`cc`/`clang`/`gcc`-compatible compiler driver on `PATH` - `kaicc` uses it
only for the final native link step - plus the platform's normal libc
development/startup files and linker support, since a bare compiler
driver package is not always sufficient by itself (see the root
`README.md`'s "Portable Linux release" section for a confirmed example).
It does **not** require LLVM to be installed on the machine running
`kaicc`.

**Output naming (WINDOWS M1):** on every platform except Windows, `<output>` is the literal produced file, with
no suffix ever added. On Windows, if `<output>` does not already end in `.exe` (case-insensitively), `kaicc`
appends `.exe` itself - so `kaicc hello.kai -o hello` produces `hello.exe`, not `hello` - rather than depending
on whichever suffixing behavior a given host compiler driver happens to apply on its own. An `<output>` that
already ends in `.exe` is never modified or double-suffixed, on any platform.

## Semantic queries

These are compiler-resolved queries against KAI's own semantic model -
symbol resolution, type information, and direct call relationships - not
textual search. Every one of them requires `--json`; there is currently no
human-readable output mode for these commands.

```
kaicc inspect <file.kai> --json
kaicc definition <file.kai> --line N --column M --json
kaicc references <file.kai> --line N --column M --json
kaicc callers <file.kai> --line N --column M --json
kaicc callees <file.kai> --line N --column M --json
kaicc call-graph <file.kai> --json
```

`--line`/`--column` are 1-indexed and must be positive integers. `inspect`
and `call-graph` operate on the whole file; the others resolve the symbol
at the given source position.

## Debug/introspection

These exist for compiler development and debugging, not as primary
end-user compilation commands:

```
kaicc --tokens <file.kai>
kaicc --ast <file.kai>
```

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | CLI usage error (malformed arguments/flags) |
| `2` | Input file could not be loaded |
| `4` | Parse failure |
| `5` | Semantic, type-checking, or control-flow failure |
| `6` | LLVM IR generation failure (includes constructs that parse/type-check but are not yet backend-lowerable, e.g. array/slice parameter or return types) |
| `7` | Native entry-point adaptation failure |
| `8` | Object file emission failure |
| `9` | No usable host C compiler driver found (`$KAI_CC`, `cc`, `clang`, `gcc`) |
| `10` | KAI runtime library (`libkai_runtime.a`) could not be located |
| `11` | Native linker invocation failed |

Semantic-query commands (`inspect`, `definition`, `references`, `callers`,
`callees`, `call-graph`) reuse the same `1`/`2`/`4`/`5` codes for their
own usage/load/parse/semantic failures - a query never partially succeeds
on a file with frontend errors.

## Environment variables

- `KAI_CC` - overrides the host C compiler driver `kaicc` uses for the
  final native link step (checked before the default `cc`/`clang`/`gcc`
  search).
- `KAI_RUNTIME_LIB` - overrides the path `kaicc` uses for
  `libkai_runtime.a`, instead of the default relative-to-`kaicc` lookup.

Both are development/override escape hatches, not something an ordinary
user needs to set.
