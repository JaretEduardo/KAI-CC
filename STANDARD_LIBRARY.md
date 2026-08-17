# KAI Standard Library

> Status: Experimental Draft
> Target version: KAI 0.1+

## 1. Overview

The KAI standard library provides fundamental functionality that is useful to most KAI programs without making that functionality part of the language syntax itself.

KAI follows this principle:

> Keep the language small and move general-purpose functionality into libraries.

The standard library should remain:

- predictable
- modular
- lightweight
- native
- inspectable
- suitable for systems programming

---

## 2. Language vs Standard Library

The language itself defines concepts such as:

- functions
- variables
- types
- ownership
- borrowing
- structs
- enums
- control flow
- modules
- generics

The standard library defines abstractions such as:

- String
- Buffer<T>
- File
- collections
- input/output
- filesystem access
- networking
- synchronization

---

## 3. Prelude

A very small set of commonly used symbols may be automatically available.

Initial candidates:

    print
    panic
    assert

Core language types such as:

    i32
    f64
    bool
    char

do not require imports.

Fundamental generic types may also eventually be automatically available:

    Option<T>
    Result<T, E>

The prelude must remain intentionally small.

---

## 4. String

Owned UTF-8 text uses:

    String

Example:

    let name = String("KAI")

String should support operations such as:

    name.len
    name.is_empty()

Future operations may include:

    push
    append
    contains
    starts_with
    ends_with
    split

String uses Move semantics.

---

## 5. Borrowed Strings

Read-only borrowed text uses:

    &str

Example:

    fn greet(name: &str) {
        print(name)
    }

String literals should behave as immutable borrowed text where practical.

---

## 6. Buffer<T>

Dynamic contiguous memory uses:

    Buffer<T>

Example:

    let data = Buffer<i32>(1024)

Expected properties:

    data.len
    data.capacity

Buffer owns its storage and uses Move semantics.

Possible operations:

    Buffer<T>(size)
    buffer.len
    buffer.is_empty()
    buffer.push(value)
    buffer.pop()
    buffer.clear()

The exact API may evolve.

---

## 7. Option<T>

Optional values use:

    Option<T>

Conceptually:

    enum Option<T> {
        Some(T)
        None
    }

KAI does not use null as the normal representation of optional values.

---

## 8. Result<T, E>

Recoverable operations use:

    Result<T, E>

Conceptually:

    enum Result<T, E> {
        Ok(T)
        Err(E)
    }

Detailed semantics are defined in:

    ERROR_MODEL.md

---

## 9. I/O

Standard input/output functionality belongs under:

    std.io

Potential APIs:

    io.print(...)
    io.println(...)
    io.read_line()

For convenience, `print` may exist in the standard prelude.

---

## 10. Filesystem

Filesystem functionality belongs under:

    std.fs

Possible API direction:

    use std.fs

    let file = fs.open("config.kai")?
    let text = file.read_text()?

Potential abstractions:

    File
    Path
    FileError

---

## 11. File Ownership

File handles follow ordinary KAI ownership rules.

Example:

    fn read() -> Result<String, FileError> {
        let file = File.open("data.txt")?
        let text = file.read_text()?

        return Ok(text)
    }

When `file` leaves scope, its operating system handle is released deterministically.

---

## 12. Mathematics

Mathematical utilities belong under:

    std.math

Potential functions:

    sqrt
    sin
    cos
    tan
    abs
    min
    max

Constants may include:

    PI
    E

Exact names are not finalized.

---

## 13. Collections

Future collection types may live under:

    std.collections

Potential types:

    List<T>
    Map<K, V>
    Set<T>
    Queue<T>

KAI 0.1 does not require all of these.

`Buffer<T>` should be sufficient for the first dynamic contiguous collection.

---

## 14. Memory

Low-level memory utilities may eventually exist under:

    std.mem

Potential functionality:

    size_of<T>()
    align_of<T>()

Raw memory manipulation should require explicit unsafe functionality in future versions.

---

## 15. Time

Future module:

    std.time

Potential types:

    Duration
    Instant
    DateTime

This is not required for KAI 0.1.

---

## 16. Networking

Future module:

    std.net

Potential abstractions:

    TcpSocket
    UdpSocket
    IpAddress

Higher-level HTTP functionality should probably remain an external package rather than part of the core standard library.

---

## 17. Concurrency

Future modules may provide:

    std.thread
    std.sync

Potential abstractions:

    Thread
    Mutex<T>
    Atomic<T>
    Channel<T>

Concurrency semantics require additional memory-model work and are not part of KAI 0.1.

---

## 18. Environment and Process

Future module:

    std.process

Potential functionality:

    process.args()
    process.exit()
    process.env()

---

## 19. Formatting

String formatting should eventually be provided by library functionality rather than complicated compiler magic where possible.

Possible future syntax:

    format("Hello {}", name)

or a future interpolation syntax.

String interpolation is not required for the first compiler milestone.

---

## 20. Iteration

Iteration should eventually be expressed through traits and library abstractions.

For KAI 0.1, arrays, ranges, and Buffer may receive compiler-supported iteration.

Example:

    for value in values {
        print(value)
    }

A general iterator system can be introduced later.

---

## 21. Allocators

The standard library may eventually expose configurable allocators.

Ordinary KAI programs should not need to interact with allocators directly.

Potential future abstractions:

    Allocator
    Arena
    Pool

Custom allocator support is not part of KAI 0.1.

---

## 22. No Mandatory Heavy Runtime

The standard library should not require a heavyweight virtual machine or mandatory garbage collector.

Programs should be able to use only the runtime facilities they require.

---

## 23. Platform Abstraction

The standard library should provide portable APIs where practical.

Platform-specific operations may exist behind dedicated modules.

Example direction:

    std.os

The language itself should remain mostly platform-independent.

---

## 24. AI-Agent Considerations

Standard library APIs should have:

- explicit argument types
- explicit return types
- explicit failure behavior
- predictable naming
- structured documentation

Example:

    pub fn open(path: &str) -> Result<File, FileError>

A coding agent can determine from the signature:

- input
- output
- failure mode
- ownership behavior

without reading the implementation.

---

## 25. Standard Library Documentation

Every public standard library symbol should eventually expose machine-readable metadata.

Possible command:

    kai inspect std.fs.File.open

Output:

    function:
        File.open

    parameters:
        path: &str

    returns:
        Result<File, FileError>

    ownership:
        returned File is owned by caller

---

## 26. Initial KAI Standard Library

The first usable KAI implementation only requires:

    print
    panic
    assert

    String / &str
    Buffer<T>

    Option<T>
    Result<T, E>

More functionality should be added incrementally.

---

## 27. Not Part of Initial Standard Library

KAI 0.1 does not need:

- HTTP framework
- JSON framework
- GUI toolkit
- database client
- GPU runtime
- machine learning framework
- web server
- package registry client

These belong to later libraries and ecosystem packages.

---

## 28. Core Standard Library Rule

The standard library should make common programming tasks convenient without turning KAI itself into a large or unpredictable language.