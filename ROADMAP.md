# KAI Roadmap

> This roadmap describes direction rather than guaranteed release dates.

---

# Phase 0 — Language Foundation

Goal: define what KAI is before implementing it.

- [x] Create KAI-CC repository
- [x] Create initial design documents
- [ ] Define KAI design principles
- [ ] Define KAI 0.1 syntax
- [ ] Define primitive types
- [ ] Define variable semantics
- [ ] Define function semantics
- [ ] Define initial memory direction
- [ ] Define compiler architecture
- [ ] Define diagnostic format
- [ ] Write example KAI programs

Exit condition:

A small set of KAI programs can be written on paper with clearly defined behavior.

---

# Phase 1 — Compiler Frontend

Goal: parse KAI source code.

- [ ] CMake project setup
- [ ] LLVM development environment
- [ ] Source manager
- [ ] Token representation
- [ ] Lexer
- [ ] Lexer tests
- [ ] Parser
- [ ] Parser tests
- [ ] AST
- [ ] AST printer
- [ ] Source spans
- [ ] Initial diagnostics

Target:

    fn main() {
        let x = 20
        let y = 22
        print(x + y)
    }

should produce a valid AST.

---

# Phase 2 — Semantic Analysis

Goal: understand and validate KAI programs.

- [ ] Symbol table
- [ ] Lexical scopes
- [ ] Primitive types
- [ ] Type inference
- [ ] Type checking
- [ ] Function signatures
- [ ] Function calls
- [ ] Return validation
- [ ] Mutability checking
- [ ] Structured semantic diagnostics
- [ ] Initial HIR

Target:

Invalid programs should fail before code generation with useful diagnostics.

---

# Phase 3 — Native Code Generation

Goal: compile the first native KAI programs.

- [ ] LLVM module generation
- [ ] Primitive value lowering
- [ ] Arithmetic
- [ ] Comparisons
- [ ] Variables
- [ ] Functions
- [ ] Function calls
- [ ] if / else
- [ ] while
- [ ] return
- [ ] print runtime function
- [ ] Object file generation
- [ ] Linking
- [ ] `kaicc` executable

Milestone:

    kaicc hello.kai -o hello
    ./hello

Output:

    Hello from KAI

This is the first major KAI milestone.

---

# Phase 4 — KAI CLI

Goal: provide a unified development workflow.

- [ ] `kai build`
- [ ] `kai run`
- [ ] `kai check`
- [ ] `kai clean`
- [ ] compiler version information
- [ ] project discovery

Example:

    kai run src/main.kai

---

# Phase 5 — Core Language

Goal: make KAI capable of writing useful programs.

Possible features:

- [ ] arrays
- [ ] strings
- [ ] structs
- [ ] modules
- [ ] imports
- [ ] basic standard library
- [ ] deterministic resource cleanup
- [ ] improved error recovery

---

# Phase 6 — Project and Package Model

Goal: support scalable KAI projects.

- [ ] `kai.toml`
- [ ] project structure
- [ ] dependency graph
- [ ] local packages
- [ ] lock file
- [ ] package resolution
- [ ] package cache

A public package registry is NOT required at this stage.

---

# Phase 7 — AI-Native Tooling

Goal: expose compiler knowledge directly to AI agents and development tools.

- [ ] JSON diagnostics
- [ ] symbol inspection
- [ ] definition lookup
- [ ] reference lookup
- [ ] type inspection
- [ ] dependency queries
- [ ] call graph
- [ ] impact analysis

Possible commands:

    kai inspect
    kai refs
    kai deps
    kai impact

Milestone:

An AI coding agent should be able to answer common semantic questions about a KAI project without scanning the entire source tree.

---

# Phase 8 — KAI IR and Optimization

Goal: introduce KAI-owned optimization infrastructure.

- [ ] MIR design
- [ ] KAI IR design
- [ ] control flow graph
- [ ] constant folding
- [ ] constant propagation
- [ ] dead code elimination
- [ ] basic inlining
- [ ] optimization reports
- [ ] benchmark suite

Optimization improvements must be measured.

---

# Phase 9 — Advanced Language Features

Candidates:

- [ ] generics
- [ ] traits
- [ ] pattern matching
- [ ] advanced error handling
- [ ] stronger ownership analysis
- [ ] concurrency primitives

Features should only be introduced after their semantics are carefully designed.

---

# Phase 10 — Performance Computing

Potential direction:

- [ ] SIMD types
- [ ] vectorization support
- [ ] parallel loops
- [ ] performance profiling
- [ ] architecture-specific optimization
- [ ] optional GPU backend research

---

# Phase 11 — Self-Hosting

Goal: implement KAI-CC in KAI.

- [ ] KAI expressive enough for compiler implementation
- [ ] compiler frontend rewritten in KAI
- [ ] semantic analysis rewritten in KAI
- [ ] compiler infrastructure rewritten in KAI
- [ ] Stage 0 bootstrap
- [ ] Stage 1 compiler
- [ ] Stage 2 self-compilation validation

Milestone:

KAI compiles the KAI compiler.

---

# Long-Term Ecosystem

Possible projects outside the core compiler:

- KAI standard library
- package registry
- language server
- debugger
- profiler
- formatter
- documentation generator
- AI agent protocol
- web frameworks
- GPU libraries
- scientific computing libraries

These projects should emerge only when the core language is stable enough to support them.