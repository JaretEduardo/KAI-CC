# KAI Error Model

> Status: Experimental Draft
> Target version: KAI 0.1+
> Last updated: August 2026

## 1. Overview

KAI uses an explicit and statically visible error model.

The language distinguishes between:

1. compile-time diagnostics
2. recoverable runtime errors
3. unrecoverable runtime failures

KAI does not use traditional exception-based error handling as its primary error mechanism.

The core goals are:

* predictable control flow
* explicit failure behavior
* low hidden runtime cost
* easy reasoning for humans
* easy reasoning for AI coding agents
* structured diagnostics
* composable error propagation
* minimal boilerplate

A function that may fail should communicate that fact through its type.

---

## 2. Error Categories

KAI distinguishes three major categories of failure.

### 2.1 Compile-Time Errors

These are errors detected by KAI-CC before the program executes.

Examples:

* syntax errors
* type mismatches
* undefined symbols
* ownership violations
* invalid borrows
* invalid function calls
* missing return values
* invalid mutation
* unhandled result values

Example:

```kai
let age: i32 = "twenty"
```

The compiler should reject the program.

---

### 2.2 Recoverable Runtime Errors

Recoverable failures are represented explicitly using:

```kai
Result<T, E>
```

Examples include:

* file not found
* network connection failure
* invalid user input
* parsing failure
* permission denied
* unavailable resource

These conditions are expected to happen during normal execution and should be handled or propagated.

---

### 2.3 Unrecoverable Runtime Failures

Failures that indicate a broken invariant or an impossible program state may terminate execution using:

```kai
panic(...)
```

Examples may include:

* internal compiler invariant violation
* unreachable program state
* corrupted runtime state
* explicit developer assertion failure

`panic` must not be used as the default mechanism for ordinary application errors.

---

# 3. No Traditional Exceptions

KAI does not use implicit exception propagation as its primary error model.

KAI 0.1 does not provide:

```text
throw
try
catch
finally
```

for normal error handling.

The following style is intentionally avoided:

```text
try {
    load_file()
} catch (...) {
    ...
}
```

because exceptions can make control flow and failure behavior less visible from function signatures.

KAI prefers:

```kai
fn load_file(path: str) -> Result<File, IOError>
```

The possibility of failure is visible directly in the type system.

---

# 4. Result<T, E>

Recoverable operations return:

```kai
Result<T, E>
```

where:

```text
T = success value
E = error value
```

Conceptually:

```kai
enum Result<T, E> {
    Ok(T)
    Err(E)
}
```

`Result<T, E>` is a fundamental standard type and should be available without importing an external package.

Example:

```kai
fn parse_number(text: str) -> Result<i32, ParseError> {
    ...
}
```

The function either produces:

```kai
Ok(42)
```

or:

```kai
Err(error)
```

---

# 5. Successful Results

A successful operation returns:

```kai
Ok(value)
```

Example:

```kai
fn divide(a: f64, b: f64) -> Result<f64, MathError> {
    if b == 0.0 {
        return Err(MathError.DivisionByZero)
    }

    return Ok(a / b)
}
```

---

# 6. Error Results

A recoverable failure returns:

```kai
Err(error)
```

Example:

```kai
fn load_user(id: u64) -> Result<User, DatabaseError> {
    if id == 0 {
        return Err(DatabaseError.InvalidId)
    }

    ...
}
```

The error value should contain useful structured information rather than only human-readable text.

---

# 7. Error Types

KAI does not require a special `error` keyword.

Errors should normally be represented using ordinary structured types such as enums and structs.

Possible future example:

```kai
enum FileError {
    NotFound
    PermissionDenied
    InvalidPath
    ReadFailed
}
```

Errors that need contextual data may store values.

Possible direction:

```kai
enum ParseError {
    InvalidCharacter(char)
    InvalidNumber(str)
    UnexpectedEnd
}
```

Exact enum syntax will be finalized in the type system specification.

---

# 8. Explicit Error Handling

A `Result<T, E>` cannot silently behave as a `T`.

Invalid:

```kai
let config = load_config("config.kai")

run(config)
```

if:

```kai
load_config(...)
```

returns:

```text
Result<Config, IOError>
```

and:

```kai
run(...)
```

requires:

```text
Config
```

KAI-CC should reject this code.

Possible diagnostic:

```text
error[E0501]: unhandled Result

 --> src/main.kai:4:5

load_config() returns:
    Result<Config, IOError>

but this operation requires:
    Config

possible actions:
    propagate the error using `?`
    handle the Result explicitly
```

---

# 9. Error Propagation Operator

KAI uses:

```text
?
```

to propagate recoverable errors.

Example:

```kai
fn load_application() -> Result<App, IOError> {
    let config = load_config("config.kai")?
    let app = create_app(config)?

    return Ok(app)
}
```

The expression:

```kai
let config = load_config("config.kai")?
```

conceptually means:

```text
result = load_config(...)

if result is Ok(value):
    continue with value

if result is Err(error):
    return Err(error)
```

The exact lowering is a compiler implementation detail.

---

# 10. Rules for `?`

The `?` operator may only be used when the current function can propagate the error.

Valid:

```kai
fn load() -> Result<Config, IOError> {
    let config = load_config("config.kai")?

    return Ok(config)
}
```

Invalid:

```kai
fn load() {
    let config = load_config("config.kai")?
}
```

because the current function does not return a compatible `Result`.

Possible diagnostic:

```text
error[E0502]: cannot propagate error

 --> src/main.kai:4:18

load_config() may return:
    IOError

current function returns:
    nothing

`?` requires a compatible Result return type.
```

---

# 11. Compatible Error Propagation

KAI 0.1 should initially require compatible error types.

Example:

```kai
fn read() -> Result<Data, IOError> {
    let file = open_file("data.txt")?

    ...
}
```

If `open_file` returns:

```text
Result<File, IOError>
```

the error can propagate directly.

Automatic conversion between unrelated error types should not occur unless explicitly defined.

This keeps propagation behavior predictable.

---

# 12. Error Conversion

Future versions may support explicit conversion between compatible error types.

Possible direction:

```kai
fn load() -> Result<Config, AppError> {
    let file = open_file("config.kai")?
    ...
}
```

where `IOError` can be explicitly converted into `AppError`.

Such conversions must remain visible and deterministic.

KAI should avoid hidden chains of implicit error conversions.

---

# 13. Explicit Result Inspection

Applications must also be able to handle `Result` values instead of propagating them.

The preferred long-term mechanism is pattern matching.

Example direction:

```kai
let result = load_config("config.kai")

match result {
    Ok(config) => {
        run(config)
    }

    Err(error) => {
        print(error)
    }
}
```

Pattern matching is not required for the earliest compiler milestone but is expected to become the primary explicit Result handling mechanism.

---

# 14. Result Convenience Operations

The standard library may eventually expose operations such as:

```text
is_ok()
is_err()
unwrap()
unwrap_or()
map()
map_err()
```

These are library-level conveniences rather than separate language constructs.

Example:

```kai
let result = parse_number("42")

if result.is_ok() {
    ...
}
```

Exact APIs are not finalized.

---

# 15. Unwrap

KAI may provide:

```kai
result.unwrap()
```

to extract a successful value when failure is considered impossible or intentionally fatal.

Example:

```kai
let value = parse_number("42").unwrap()
```

If the Result contains an error, execution panics.

`unwrap()` should be used deliberately.

AI tooling and compiler linting may warn when `unwrap()` is used unnecessarily in production code.

---

# 16. Panic

KAI provides:

```kai
panic(message)
```

for unrecoverable failures.

Example:

```kai
fn impossible_state() {
    panic("invalid internal state")
}
```

A panic terminates normal control flow.

The exact runtime panic behavior is implementation-defined during early versions.

The runtime should at minimum report:

* panic message
* source location when available
* function or stack information when available

---

# 17. Panic Is Not Normal Error Handling

This should be discouraged:

```kai
fn open_config(path: str) -> Config {
    if file_missing(path) {
        panic("file not found")
    }

    ...
}
```

Prefer:

```kai
fn open_config(path: str) -> Result<Config, IOError> {
    ...
}
```

A missing file can happen during normal execution and is therefore recoverable.

---

# 18. Assertions

KAI may provide assertions for validating programmer assumptions.

Example:

```kai
assert(value >= 0)
```

Failure causes a panic.

Possible extended form:

```kai
assert(value >= 0, "value must not be negative")
```

Assertions are primarily intended for:

* development
* debugging
* invariant checking
* tests

Exact assertion behavior may be implemented through the standard library or compiler intrinsics.

---

# 19. Unreachable States

KAI may eventually provide:

```kai
unreachable()
```

for code paths that should never execute.

Example:

```kai
if impossible_condition {
    unreachable()
}
```

This should be used sparingly.

The compiler may use unreachable information for optimization.

---

# 20. Unit Success Values

Some operations may succeed without returning meaningful data.

KAI may use the unit type:

```text
()
```

for this purpose.

Example:

```kai
fn save(config: &Config) -> Result<(), IOError> {
    ...

    return Ok(())
}
```

`()` represents a value containing no application data.

The exact unit syntax should be finalized in `TYPE_SYSTEM.md`.

---

# 21. Ownership and Result

`Result<T, E>` participates normally in KAI's ownership system.

If `T` or `E` contains owned resources, the Result owns them.

Example:

```kai
fn open() -> Result<File, IOError>
```

A successful:

```kai
Ok(file)
```

owns the returned `File`.

When the caller extracts the file, ownership transfers appropriately.

KAI must not duplicate owned resources implicitly when handling Results.

---

# 22. Propagation and Ownership

Consider:

```kai
fn load() -> Result<Buffer<i32>, IOError> {
    let data = read_buffer()?

    return Ok(data)
}
```

Ownership flow:

```text
read_buffer()
      |
      | Ok(Buffer)
      v
    data
      |
      | move
      v
 Ok(data)
      |
      | return ownership
      v
    caller
```

If `read_buffer()` returns an error, no invalid resource ownership should remain.

---

# 23. Cleanup During Error Propagation

Deterministic cleanup still applies when a function returns early because of `?`.

Example:

```kai
fn process() -> Result<Data, IOError> {
    let temp = Buffer<i32>(1024)

    let file = open_file("data.txt")?

    ...
}
```

If `open_file()` returns an error:

1. the error is prepared for propagation
2. owned local resources such as `temp` are destroyed
3. the function returns the error

This behavior must be deterministic.

The developer should not need manual cleanup.

---

# 24. Error Values Should Be Structured

KAI discourages using arbitrary strings as the only representation of recoverable errors.

Less desirable:

```kai
Err("something went wrong")
```

Preferred:

```kai
Err(FileError.NotFound)
```

or a structured equivalent containing relevant data.

Human-readable messages can be generated from structured errors.

Structured errors are easier for:

* programs
* IDEs
* tests
* AI agents
* telemetry
* documentation

to inspect reliably.

---

# 25. Compiler Diagnostics Are Different From Program Errors

KAI-CC compiler diagnostics are not `Result<T, E>` values from the user program.

For example:

```kai
let age: i32 = "twenty"
```

produces a compiler diagnostic because the program itself is invalid.

This is different from:

```kai
let age = parse_age(input)?
```

where malformed user input is a valid runtime possibility.

The compiler error system and program error system are conceptually separate.

---

# 26. Compiler Diagnostic Structure

Every KAI-CC diagnostic should have a stable structure.

Conceptual representation:

```text
Diagnostic {
    code
    severity
    category
    message
    primary_span
    secondary_spans
    notes
    suggestions
}
```

Possible severity values:

```text
error
warning
note
help
```

---

# 27. Stable Diagnostic Codes

Compiler errors should have stable identifiers.

Example:

```text
E0201
```

A diagnostic code should identify a category of compiler failure rather than a unique source location.

Possible initial ranges:

```text
E01xx  lexical errors
E02xx  syntax errors
E03xx  type errors
E04xx  ownership and borrowing
E05xx  Result and error handling
E06xx  name and symbol resolution
E07xx  module errors
```

The exact numbering scheme may evolve before KAI becomes stable.

---

# 28. Human-Readable Diagnostics

Example:

```text
error[E0501]: unhandled Result

 --> src/main.kai:8:15

8 | let config = load_config("config.kai")
                 ^^^^^^^^^^^^^^^^^^^^^^^^

returns:
    Result<Config, IOError>

expected:
    Config

help:
    propagate the error with `?`
```

Diagnostics should be concise but contain enough information to understand and fix the problem.

---

# 29. Machine-Readable Diagnostics

KAI-CC should provide structured diagnostics.

Example command:

```text
kai check --json
```

Possible output:

```json
{
    "code": "E0501",
    "severity": "error",
    "kind": "unhandled_result",
    "message": "Result value must be handled or propagated",
    "location": {
        "file": "src/main.kai",
        "line": 8,
        "column": 15
    },
    "types": {
        "found": "Result<Config, IOError>",
        "expected": "Config"
    },
    "actions": [
        {
            "kind": "propagate",
            "operator": "?"
        },
        {
            "kind": "handle"
        }
    ]
}
```

The machine-readable representation should not require parsing human diagnostic text.

---

# 30. AI-Agent Diagnostics

KAI diagnostics are explicitly designed to support automated coding agents.

A diagnostic should answer, when possible:

1. What failed?
2. Where did it fail?
3. What rule was violated?
4. What was expected?
5. What was found?
6. What source locations contributed to the failure?
7. What valid categories of fixes exist?

Diagnostics should avoid forcing an AI agent to infer these facts from unstructured compiler output.

---

# 31. Suggestions Must Be Safe

Compiler suggestions should describe valid transformations when the compiler can prove them.

Example:

```text
help:
    use `?` to propagate IOError
```

The compiler should not automatically propose large semantic changes that may alter program intent.

For example, an error should not blindly suggest:

```text
change the function return type
```

unless the consequences are clear.

AI agents may choose broader refactors, but KAI-CC should provide precise semantic information rather than guess developer intent.

---

# 32. Warnings

Warnings represent valid programs with potentially undesirable behavior.

Possible examples:

* unused value
* unused mutable binding
* unreachable code
* unnecessary clone
* unnecessary mutable borrow
* ignored Result
* suspicious conversion

Example:

```text
warning[W0401]: unnecessary clone

data.clone()
     ^^^^^^^

`data` is not used after this operation and can be moved instead.
```

Warning codes should also be machine-readable.

---

# 33. Ignoring Results

A `Result` should not be silently discarded accidentally.

Potentially problematic:

```kai
save_config(config)
```

when:

```kai
save_config(...)
```

returns:

```text
Result<(), IOError>
```

KAI should warn or error when a Result is ignored.

The exact severity is not finalized.

Explicit discard syntax may be introduced if needed.

Possible future direction:

```kai
_ = save_config(config)
```

This makes intentional ignoring visible.

---

# 34. Main Function and Errors

KAI should eventually allow the program entry point to return a Result.

Possible direction:

```kai
fn main() -> Result<(), AppError> {
    let config = load_config("config.kai")?

    run(config)

    return Ok(())
}
```

If `main()` returns an error, the runtime should:

* print a useful error representation
* return a non-zero process exit code
* preserve structured information where practical

Exact behavior will be defined later.

---

# 35. Error Context

Errors often need additional context while propagating.

Future standard library functionality may allow:

```kai
let config = load_config(path)
    .context("failed to load application configuration")?
```

The language should prefer structured context rather than repeatedly converting errors into plain strings.

The exact API is not part of KAI 0.1.

---

# 36. Error Chaining

Future error values may preserve their underlying cause.

Conceptually:

```text
AppError
   |
   +-- caused by IOError
         |
         +-- PermissionDenied
```

This allows humans and tools to inspect both:

* high-level application context
* low-level root cause

Error chaining should remain structured.

---

# 37. No Hidden Global Error State

KAI should not depend on a hidden global error variable similar to:

```text
errno
```

for ordinary language-level error handling.

Platform interfaces may expose operating system error state internally, but KAI APIs should convert those failures into explicit error values when practical.

---

# 38. FFI Errors

Foreign function interfaces may use error conventions that differ from KAI.

Examples include:

* integer return codes
* null pointers
* operating system error state
* C-style output parameters

KAI wrappers should convert these conventions into:

```text
Result<T, E>
```

whenever practical.

Raw FFI behavior may require `unsafe` in future versions.

---

# 39. Panic Across FFI

KAI should not allow a panic to unwind unpredictably across foreign ABI boundaries.

Exact FFI panic behavior will be defined later.

Initial direction:

* panic must be contained before crossing an FFI boundary
* or program execution terminates

This protects foreign runtimes from undefined behavior.

---

# 40. Error Handling and Performance

`Result<T, E>` should not require heap allocation by default.

The compiler should represent Results efficiently using ordinary values.

Error handling should favor predictable control flow and zero-cost success paths where practical.

KAI should not require a heavyweight runtime exception system for recoverable errors.

---

# 41. Error Handling and Token Efficiency

KAI's error model should remain compact.

Example:

```kai
let config = load_config(path)?
```

communicates both:

* the operation may fail
* the failure should propagate

without requiring large amounts of boilerplate.

However, token efficiency must not hide important control flow.

KAI should prefer concise explicit behavior over implicit exceptions.

---

# 42. Error Handling and Semantic Tooling

Future KAI tooling should be able to answer questions such as:

```text
kai errors load_config
kai callers load_config
kai inspect IOError
```

Possible output:

```text
function:
    load_config

returns:
    Result<Config, IOError>

possible errors:
    IOError.NotFound
    IOError.PermissionDenied
    IOError.InvalidFormat

propagated by:
    src/app.kai:18
    src/server.kai:42

handled by:
    src/cli.kai:27
```

This allows an AI agent to understand failure behavior without reading every implementation.

---

# 43. Result Flow Analysis

KAI-CC may eventually track how errors move through a program.

For example:

```text
read_file
    |
    v
IOError
    |
    v
load_config
    |
    v
ConfigError
    |
    v
main
```

Such information can power:

* impact analysis
* API documentation
* debugging
* automated refactoring
* AI-agent semantic queries

This is a long-term tooling goal.

---

# 44. KAI 0.1 Restrictions

To keep the initial language manageable, KAI 0.1 will not require:

* traditional exceptions
* stack unwinding for recoverable errors
* resumable exceptions
* checked exception syntax
* automatic conversion between arbitrary error types
* complex error hierarchies
* error inheritance
* panic recovery
* asynchronous error propagation
* advanced error chaining
* automatic retry behavior

These features may be considered individually if real programs demonstrate a need for them.

---

# 45. Initial Compiler Requirements

The initial compiler should eventually detect:

* using `Result<T, E>` where `T` is required
* invalid use of `?`
* incompatible propagated error types
* missing return paths
* invalid Result construction
* ignored Results where required by language rules

The earliest native-code milestone may implement only a subset of the complete error model.

---

# 46. First Implementation Scope

Error handling should be introduced incrementally.

Suggested implementation order:

```text
Stage 1
compile-time diagnostics only

Stage 2
Result<T, E>

Stage 3
Ok / Err construction

Stage 4
explicit Result handling

Stage 5
? propagation

Stage 6
panic and assertions

Stage 7
structured JSON diagnostics
```

The design may be specified before every stage is implemented.

---

# 47. Example: File Loading

```kai
fn load_text(path: str) -> Result<String, IOError> {
    let file = File.open(path)?
    let text = file.read_text()?

    return Ok(text)
}

fn load_config(path: str) -> Result<Config, ConfigError> {
    let text = load_text(path)?
    let config = parse_config(text)?

    return Ok(config)
}
```

This example assumes compatible error conversion exists.

Until such conversion is implemented, functions should use directly compatible error types or convert them explicitly.

---

# 48. Example: Explicit Handling

Future syntax:

```kai
fn start() {
    let result = load_config("config.kai")

    match result {
        Ok(config) => {
            run(config)
        }

        Err(error) => {
            print(error)
        }
    }
}
```

The Result is explicitly consumed by the `match`.

---

# 49. Example: Propagation

```kai
fn start() -> Result<(), AppError> {
    let config = load_config("config.kai")?

    run(config)?

    return Ok(())
}
```

The `?` operator allows concise propagation while keeping failure visible in the function signature.

---

# 50. Example: Panic

```kai
fn compiler_internal_operation(node: Node) {
    if node.is_invalid_internal_state() {
        panic("compiler invariant violated")
    }
}
```

This is appropriate because the condition represents a bug or broken invariant rather than an expected runtime event.

---

# 51. Design Goals Summary

KAI's error model should provide:

* explicit failure in function signatures
* structured recoverable errors
* `Result<T, E>`
* concise propagation using `?`
* deterministic cleanup during propagation
* no mandatory exception runtime
* panic for unrecoverable states
* structured compiler diagnostics
* machine-readable diagnostic output
* predictable behavior for AI agents
* minimal boilerplate

---

# 52. Open Questions

The following decisions remain intentionally unresolved:

* exact enum syntax
* exact `match` syntax
* whether `Result` is compiler-known or purely standard library
* exact unit type syntax
* error conversion mechanism
* whether ignored Results are warnings or hard errors
* whether `unwrap()` is always available
* whether production builds may restrict `unwrap()`
* panic implementation strategy
* whether panic supports stack unwinding
* whether KAI exposes backtraces by default
* how `main() -> Result` maps errors to exit codes
* structured application error formatting
* future error-context APIs
* FFI error conversion conventions

These questions should be resolved only when required by implementation or real KAI programs.

---

# 53. Core Error Rule

If an operation can fail during normal execution, that possibility should be visible in its type.

KAI should make failure behavior explicit enough that a human or AI agent can understand how an error enters, propagates through, and leaves a function without reading hidden runtime behavior.
