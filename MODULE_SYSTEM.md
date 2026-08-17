# KAI Module System

> Status: Experimental Draft
> Target version: KAI 0.1+

## 1. Overview

KAI uses a simple file-based module system.

The module system is designed to:

- scale to large projects
- minimize boilerplate
- make symbol origin obvious
- provide predictable name resolution
- support libraries and frameworks
- expose dependency information to development tools and AI agents

KAI avoids implicit global namespaces and wildcard imports by default.

---

## 2. File-Based Modules

Every `.kai` source file defines one module.

Example project:

    src/
    ├── main.kai
    ├── user.kai
    ├── auth.kai
    └── net/
        ├── http.kai
        └── socket.kai

Modules are derived from their paths:

    main
    user
    auth
    net.http
    net.socket

KAI source files do not need to repeat their module name.

For example:

    src/net/http.kai

automatically represents:

    net.http

This avoids unnecessary boilerplate such as:

    module net.http

---

## 3. Package Root

The `src/` directory is the default source root of a KAI package.

Example:

    my_project/
    ├── kai.toml
    └── src/
        ├── main.kai
        └── auth.kai

Module paths are resolved relative to `src/`.

---

## 4. Importing Modules

Modules are imported using:

    use

Example:

    use net.http

The module name remains visible:

    let server = http.Server()

KAI should prefer qualified access because it makes symbol origin obvious.

---

## 5. Importing Symbols

Individual public symbols may also be imported.

Example:

    use net.http.Server

Then:

    let server = Server()

This should be used when the shorter form improves readability without creating ambiguity.

---

## 6. No Wildcard Imports by Default

KAI 0.1 does not support:

    use net.http.*

Wildcard imports make symbol origin less obvious and increase ambiguity for humans and AI tools.

They may be reconsidered later if a compelling use case appears.

---

## 7. Visibility

Declarations are private by default.

Example:

    fn hash_password(password: &str) -> String {
        ...
    }

This function is only visible inside its module.

Public declarations use:

    pub

Example:

    pub fn login(user: &User) -> Result<Session, AuthError> {
        ...
    }

Public structs:

    pub struct User {
        id: u64
        name: String
    }

---

## 8. Public Struct Fields

Struct fields are private by default.

Example:

    pub struct User {
        id: u64
        name: String
    }

The struct itself is public, but its fields are not automatically public.

Public fields require:

    pub struct User {
        pub id: u64
        pub name: String
    }

This prevents accidental exposure of implementation details.

---

## 9. Name Resolution

When resolving a symbol, KAI checks predictable scopes.

Initial resolution order:

1. local variables
2. function parameters
3. declarations in the current module
4. explicitly imported symbols
5. explicitly imported modules
6. standard prelude symbols

KAI should not search unrelated modules automatically.

---

## 10. Ambiguous Names

If two imported symbols use the same name, KAI must reject unqualified use.

Example:

    use graphics.Point
    use geometry.Point

Then:

    let p = Point()

is ambiguous.

The programmer should preserve module qualification or use an alias.

---

## 11. Import Aliases

Future or early syntax:

    use net.http as http
    use database.postgres as pg

Then:

    let server = http.Server()
    let db = pg.connect()

Aliases should only rename modules or imported symbols.

They must not change their semantics.

---

## 12. Circular Dependencies

KAI should detect module dependency cycles.

Example:

    auth -> user
    user -> auth

A dependency cycle should produce a structured diagnostic when it prevents semantic resolution.

The compiler should report the complete cycle.

Example:

    error[E0701]: circular module dependency

    auth
      -> user
      -> permissions
      -> auth

---

## 13. Module Initialization

KAI modules do not execute arbitrary code merely because they are imported.

Importing a module should not introduce hidden runtime initialization.

Global initialization semantics may be designed separately in the future.

This makes dependency behavior predictable.

---

## 14. Entry Point

Executable packages use:

    src/main.kai

and contain:

    fn main() {
        ...
    }

The compiler treats `main` as the executable entry point.

---

## 15. Library Packages

Libraries may use:

    src/lib.kai

as their public root.

Example:

    src/
    ├── lib.kai
    ├── parser.kai
    └── lexer.kai

The exact package export mechanism will evolve with the package system.

---

## 16. Public API Re-Exports

Future versions may support:

    pub use parser.Parser

This allows libraries to expose selected symbols through a stable public API without exposing their internal directory structure.

This feature is not required for the first compiler milestone.

---

## 17. Standard Library Modules

Standard library modules use the `std` namespace.

Examples:

    use std.io
    use std.fs
    use std.math

Third-party packages use their package name.

Example:

    use http.server

Exact external package resolution is defined by the project/package model.

---

## 18. Semantic Dependency Graph

KAI-CC should internally construct a module dependency graph.

Example:

    app
    ├── auth
    │   ├── crypto
    │   └── user
    └── database

This graph should eventually power tooling such as:

    kai deps
    kai impact
    kai modules

---

## 19. AI-Agent Considerations

The module system should make symbol origin easy to discover.

For example:

    use database.User

communicates more information than an implicit global import.

Future tooling:

    kai where User

Possible output:

    symbol:
        User

    defined:
        src/database/user.kai:4

    module:
        database.user

    imported by:
        7 modules

---

## 20. Machine-Readable Module Information

Future semantic tooling should expose module information structurally.

Example:

    {
        "module": "auth",
        "file": "src/auth.kai",
        "imports": [
            "database.user",
            "crypto.hash"
        ],
        "exports": [
            "login",
            "logout"
        ]
    }

---

## 21. KAI 0.1 Rules

KAI 0.1 uses:

- one module per `.kai` file
- module path derived from file path
- `use` for imports
- `pub` for public visibility
- private-by-default declarations
- explicit imports
- no wildcard imports
- no implicit module initialization

---

## 22. Open Questions

Future decisions include:

- exact import alias syntax
- public re-exports
- nested package visibility
- friend/package-private visibility
- cyclic dependency rules
- build-time generated modules
- conditional compilation
- platform-specific modules

---

## 23. Core Module Rule

KAI module relationships should be explicit enough that a human or AI agent can identify where a symbol comes from without searching the complete codebase.