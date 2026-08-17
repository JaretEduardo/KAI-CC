# KAI Project and Package Model

> Status: Experimental Draft
> Target version: KAI 0.1+

## 1. Overview

KAI projects use a standardized structure and a single project manifest.

The goals are:

- predictable project layout
- reproducible builds
- simple dependency management
- minimal configuration
- easy navigation for humans and AI agents

The primary project manifest is:

    kai.toml

---

## 2. Basic Executable Project

Example:

    hello/
    ├── kai.toml
    ├── src/
    │   └── main.kai
    └── tests/

---

## 3. Library Project

Example:

    mathlib/
    ├── kai.toml
    ├── src/
    │   ├── lib.kai
    │   └── vector.kai
    └── tests/

---

## 4. Project Manifest

Initial format:

    [package]
    name = "hello"
    version = "0.1.0"

Example with metadata:

    [package]
    name = "kai-http"
    version = "0.1.0"
    description = "HTTP library for KAI"
    license = "MIT"

Not every field is required for local projects.

---

## 5. Dependencies

Future dependency declaration:

    [dependencies]
    json = "1.2.0"
    http = "0.4.0"

Dependencies may come from:

- package registry
- local filesystem
- Git repository

Registry support is not required for the first compiler milestone.

---

## 6. Local Dependencies

Development should support local packages.

Example direction:

    [dependencies]
    utils = { path = "../utils" }

This is useful before a public package registry exists.

---

## 7. Git Dependencies

Possible future form:

    [dependencies]
    parser = { git = "...", rev = "..." }

Exact syntax is not finalized.

---

## 8. Lock File

Dependency resolution should eventually generate:

    kai.lock

The lock file records exact dependency versions.

Applications should normally commit `kai.lock`.

Library policy may be defined later.

---

## 9. Source Directory

Application and library source files live under:

    src/

The compiler should not require manually listing every `.kai` file.

It discovers modules from the source tree.

---

## 10. Tests

Tests live under:

    tests/

Example:

    tests/
    ├── parser.kai
    └── auth.kai

Unit-test syntax will be designed later.

The initial compiler does not require built-in testing syntax.

---

## 11. Examples

Libraries may provide:

    examples/

Example:

    examples/
    └── basic_server.kai

These are normal KAI programs demonstrating package usage.

---

## 12. Build Output

Generated build artifacts should live under:

    target/

Example:

    target/
    ├── debug/
    └── release/

The exact structure may change.

---

## 13. Standard CLI

Developers should primarily interact with:

    kai

Commands should eventually include:

    kai new
    kai build
    kai run
    kai check
    kai test
    kai fmt
    kai clean

Package commands may include:

    kai add
    kai remove
    kai update

---

## 14. Creating a Project

Future command:

    kai new hello

Produces:

    hello/
    ├── kai.toml
    └── src/
        └── main.kai

with:

    fn main() {
        print("Hello from KAI")
    }

---

## 15. Build Profiles

Initial build profiles:

    debug
    release

Example:

    kai build

uses debug mode.

    kai build --release

uses release mode.

The exact optimization levels are compiler implementation details.

---

## 16. Reproducibility

Given the same:

- source code
- compiler version
- manifest
- lock file
- target configuration

KAI should aim to produce reproducible builds where practical.

---

## 17. Package Naming

Package names should be:

- concise
- lowercase
- predictable

The exact allowed character rules are not yet finalized.

Module paths and package names are separate concepts.

---

## 18. Package Namespace

External modules are namespaced under their package identity.

A package should not silently inject symbols into another package's root namespace.

This prevents dependency collisions.

---

## 19. Semantic Project Index

KAI tooling should understand the entire project as a semantic graph.

Potential information:

    files
    modules
    symbols
    dependencies
    call graph
    type graph
    references

This information should power both developer tooling and AI-agent tooling.

---

## 20. AI-Agent Project Queries

Future commands:

    kai project
    kai modules
    kai deps
    kai refs
    kai impact

Example:

    $ kai project

    package:
        my-app

    modules:
        18

    public symbols:
        42

    dependencies:
        3

    entry:
        src/main.kai

This can reduce the need for AI agents to scan the entire repository.

---

## 21. Package Registry

A public KAI package registry is a long-term ecosystem feature.

It is not required for KAI 0.1.

The compiler and project model should not depend on the existence of a central registry.

Local development must remain possible without internet access.

---

## 22. Workspaces

Future KAI versions may support multi-package workspaces.

Example:

    workspace/
    ├── kai.toml
    ├── compiler/
    ├── runtime/
    └── tools/

This will become useful for large KAI projects, including eventually KAI-CC itself.

---

## 23. Tool Configuration

Tool configuration should preferably live inside `kai.toml` rather than creating many unrelated configuration files.

Future sections may include:

    [format]
    [lint]
    [build]
    [target]

Configuration should remain optional when sensible defaults exist.

---

## 24. KAI 0.1 Project Requirements

The first compiler only needs:

    project/
    └── src/
        └── main.kai

and eventually a basic:

    kai.toml

Dependency resolution is not required for the first native executable.

---

## 25. Core Project Rule

A developer or AI agent should be able to understand the structure of a KAI project primarily from its directory layout and `kai.toml`, without reverse-engineering custom build scripts.