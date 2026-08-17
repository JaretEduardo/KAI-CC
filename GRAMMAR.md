# KAI Grammar

> Status: KAI 0.1 Draft
> Notation: simplified EBNF

## 1. Overview

This document defines the initial grammar accepted by the KAI compiler frontend.

The grammar intentionally covers only the early KAI language.

Later language features should extend this document rather than bypassing it.

---

## 2. Lexical Elements

### Identifiers

    identifier =
        letter { letter | digit | "_" }

Examples:

    user
    load_user
    value2
    Buffer

Identifiers are case-sensitive.

---

## 3. Keywords

Initial reserved keywords:

    fn
    let
    mut
    return

    if
    else

    while
    for
    in

    struct
    enum

    use
    pub

    true
    false

Future keywords may include:

    match
    impl
    trait
    unsafe

---

## 4. Literals

    integer_literal
    float_literal
    string_literal
    char_literal
    boolean_literal

Boolean:

    boolean_literal =
        "true" | "false"

---

## 5. Comments

Single-line comments:

    // comment

A comment continues until the end of the line.

Block comments are not required for KAI 0.1.

---

## 6. Statement Termination

KAI does not require semicolons.

A statement normally ends at:

- a newline
- a closing block `}`
- the end of the file

Semicolons may optionally be accepted as explicit separators.

Inside:

    ()
    []
    {}

line breaks may be accepted where the grammar remains unambiguous.

Exact newline handling is an implementation detail of the lexer/parser.

---

# 7. Program

    program =
        { declaration }

---

# 8. Declaration

    declaration =
          function_declaration
        | struct_declaration
        | enum_declaration
        | import_declaration

---

# 9. Function Declaration

    function_declaration =
        [ "pub" ]
        "fn"
        identifier
        "("
        [ parameter_list ]
        ")"
        [ "->" type ]
        block

---

# 10. Parameters

    parameter_list =
        parameter { "," parameter }

    parameter =
        identifier ":" type

Example:

    fn add(a: i32, b: i32) -> i32 {
        return a + b
    }

---

# 11. Type

Initial grammar:

    type =
          primitive_type
        | named_type
        | reference_type
        | array_type
        | generic_type
        | unit_type

---

# 12. Primitive Types

    primitive_type =
          "i8"
        | "i16"
        | "i32"
        | "i64"
        | "u8"
        | "u16"
        | "u32"
        | "u64"
        | "f32"
        | "f64"
        | "bool"
        | "char"

---

# 13. Named Type

    named_type =
        identifier

Examples:

    String
    User
    FileError

---

# 14. Reference Type

    reference_type =
          "&" type
        | "&" "mut" type

Examples:

    &User
    &str
    &mut Buffer

---

# 15. Array Type

    array_type =
        "[" type ";" integer_literal "]"

Example:

    [i32; 4]

---

# 16. Slice Type

Slices appear through references:

    &[T]
    &mut [T]

Conceptual grammar extension:

    slice_type =
        "[" type "]"

A bare slice type cannot normally exist as a local owned value.

---

# 17. Generic Type

    generic_type =
        identifier "<" type_list ">"

    type_list =
        type { "," type }

Examples:

    Buffer<i32>
    Option<User>
    Result<User, IOError>

---

# 18. Unit Type

    unit_type =
        "(" ")"

---

# 19. Block

    block =
        "{"
        { statement }
        "}"

---

# 20. Statement

    statement =
          variable_declaration
        | expression_statement
        | return_statement
        | if_statement
        | while_statement
        | for_statement
        | block

---

# 21. Immutable Variable

    variable_declaration =
          "let" identifier [ ":" type ] "=" expression
        | "mut" identifier [ ":" type ] "=" expression

Examples:

    let x = 10
    let x: i64 = 10

    mut count = 0
    mut count: u64 = 0

---

# 22. Return

    return_statement =
        "return" [ expression ]

Examples:

    return
    return 42
    return Ok(value)

---

# 23. If Statement

    if_statement =
        "if" expression block
        { "else" "if" expression block }
        [ "else" block ]

Example:

    if x > 0 {
        print("positive")
    } else {
        print("other")
    }

---

# 24. While Statement

    while_statement =
        "while" expression block

Example:

    while i < 10 {
        i = i + 1
    }

---

# 25. For Statement

    for_statement =
        "for"
        identifier
        "in"
        expression
        block

Example:

    for i in 0..10 {
        print(i)
    }

---

# 26. Expressions

Expression precedence from lowest to highest:

    assignment
    logical_or
    logical_and
    equality
    comparison
    range
    additive
    multiplicative
    unary
    postfix
    primary

---

# 27. Assignment

    assignment =
        logical_or
        [ "=" assignment ]

Assignment targets must be valid mutable locations.

Example:

    count = count + 1

---

# 28. Logical OR

    logical_or =
        logical_and { "||" logical_and }

---

# 29. Logical AND

    logical_and =
        equality { "&&" equality }

---

# 30. Equality

    equality =
        comparison { ( "==" | "!=" ) comparison }

---

# 31. Comparison

    comparison =
        range {
            ( "<" | "<=" | ">" | ">=" )
            range
        }

---

# 32. Range

    range =
        additive [ ".." additive ]

Example:

    0..10

The upper bound is exclusive.

Future syntax may include:

    0..=10

but inclusive ranges are not required for the initial compiler.

---

# 33. Addition

    additive =
        multiplicative {
            ( "+" | "-" )
            multiplicative
        }

---

# 34. Multiplication

    multiplicative =
        unary {
            ( "*" | "/" | "%" )
            unary
        }

---

# 35. Unary

    unary =
          "!" unary
        | "-" unary
        | "&" unary
        | "&" "mut" unary
        | postfix

---

# 36. Postfix Expressions

    postfix =
        primary {
              function_call
            | member_access
            | indexing
            | error_propagation
        }

---

# 37. Function Calls

    function_call =
        "(" [ argument_list ] ")"

    argument_list =
        expression { "," expression }

Example:

    add(10, 20)

---

# 38. Member Access

    member_access =
        "." identifier

Examples:

    user.name
    data.len
    File.open

---

# 39. Indexing

    indexing =
        "[" expression "]"

Example:

    values[0]

---

# 40. Error Propagation

    error_propagation =
        "?"

Example:

    load_config(path)?

---

# 41. Primary Expressions

    primary =
          integer_literal
        | float_literal
        | string_literal
        | char_literal
        | boolean_literal
        | identifier
        | array_literal
        | struct_literal
        | "(" expression ")"

---

# 42. Array Literal

    array_literal =
        "["
        [ expression { "," expression } ]
        "]"

Example:

    [1, 2, 3, 4]

---

# 43. Struct Declaration

    struct_declaration =
        [ "pub" ]
        "struct"
        identifier
        "{"
        { field_declaration }
        "}"

    field_declaration =
        [ "pub" ]
        identifier
        ":"
        type

Example:

    struct User {
        id: u64
        name: String
        active: bool
    }

---

# 44. Struct Literal

    struct_literal =
        identifier
        "{"
        field_initializer
        { "," field_initializer }
        [ "," ]
        "}"

    field_initializer =
        identifier ":" expression

Example:

    User {
        id: 1,
        name: String("KAI"),
        active: true
    }

---

# 45. Enum Declaration

Initial proposed grammar:

    enum_declaration =
        [ "pub" ]
        "enum"
        identifier
        "{"
        enum_variant
        { enum_variant }
        "}"

    enum_variant =
        identifier
        [ "(" type_list ")" ]

Examples:

    enum Direction {
        North
        South
        East
        West
    }

    enum Option<T> {
        Some(T)
        None
    }

Generic enum declaration grammar will be finalized when user-defined generics are implemented.

---

# 46. Imports

    import_declaration =
        "use"
        module_path
        [ "as" identifier ]

    module_path =
        identifier { "." identifier }

Examples:

    use std.io
    use net.http.Server
    use database.postgres as pg

---

# 47. Public Visibility

`pub` may initially appear before:

    fn
    struct
    enum

and struct fields.

Additional visibility rules may be added later.

---

# 48. Unsupported KAI 0.1 Grammar

The initial parser does not need:

    class
    inheritance
    try
    catch
    throw
    async
    await
    macro
    unsafe
    impl
    trait
    match

Some of these are planned for later versions.

---

# 49. Parser Strategy

The initial KAI parser should use recursive descent.

Expressions should use precedence-based recursive descent or Pratt parsing.

The parser must produce source spans for every AST node where practical.

---

# 50. Grammar Rule

The KAI grammar should prioritize predictable parsing over clever or highly context-sensitive syntax.