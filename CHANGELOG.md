# Changelog

All notable KAI-CC releases are documented here. See
[`docs/releases/`](docs/releases/) for the full notes of each release.

## 0.1.0-alpha.1

First public pre-release of the KAI reference compiler. A complete,
working, end-to-end pipeline (lexer, parser, semantic analysis, type
checking, control-flow analysis, LLVM codegen, native object emission and
linking) compiles a real subset of KAI source to a native Linux x86_64
executable. Includes spellable `str` parameters/returns, compiler-resolved
semantic tooling (`inspect`/`definition`/`references`/`callers`/`callees`/
`call-graph`), a portable Linux x86_64 binary release, and a VS Code
extension. Licensed under Apache License 2.0. See
[`docs/releases/0.1.0-alpha.1.md`](docs/releases/0.1.0-alpha.1.md) for
full details, including known limitations.
