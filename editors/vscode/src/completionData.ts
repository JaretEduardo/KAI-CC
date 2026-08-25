// VS CODE MILESTONE 3: pure, vscode-API-free KAI Basic IntelliSense
// metadata and context logic - kept separate from completions.ts (which
// builds actual vscode.CompletionItem objects) so this stays directly
// unit-testable with plain Node, the same pattern paths.ts/process.ts
// established in Milestone 2 (`vscode` cannot be `require()`d outside a
// running extension host).
//
// This is intentionally NOT a Language Server and does NOT attempt
// semantic completion (no user-declared locals/functions, no hover, no
// go-to-definition - see this milestone's own §2).
//
// Metadata below is verified against the CURRENT frontend, not guessed:
//   - keywords: compiler/src/lexer/Lexer.cpp's own kKeywords table
//     (compiler/include/kai/lexer/TokenKind.hpp's Kw* enumerators)
//   - primitive types: compiler/include/kai/semantic/Type.hpp's TypeKind
//     (Unresolved/Error/Unit excluded - those are internal compiler
//     states, never valid KAI source type-annotation spellings)
//   - builtins: compiler/src/semantic/SemanticAnalyzer.cpp's prelude
//     ("print", "panic", "assert") - only `print` is included here since
//     only `print` has real M6 native/runtime lowering (see
//     compiler/src/codegen/LLVMExpressionLowering.cpp's lowerPrintCall());
//     `panic`/`assert` exist as recognized-but-unsupported Builtin
//     symbols and would fail cleanly if actually called, so they are
//     deliberately NOT advertised as usable MVP functions here.
//
// `str` is deliberately excluded from primitive-type completion: it is
// highlighted by the TextMate grammar (because example programs use
// string literals) and is a real *lexical* literal kind, but KAI's
// semantic Type vocabulary has no TypeKind::String at all yet (a string
// literal's own semantic Type is Type::unresolved() - see TypeChecker.cpp
// and the M6 report), so presenting `str` as an ordinary, fully-supported
// primitive type alongside `i64`/`bool`/etc. would mislead a beginner.

export interface CompletionMetadata {
    readonly label: string;
    readonly detail: string;
}

/** The COMPLETE current lexer keyword set (Lexer.cpp's kKeywords table) - kept in that same order. */
export const KEYWORDS: readonly CompletionMetadata[] = [
    { label: 'fn', detail: 'Declare a function' },
    { label: 'let', detail: 'Declare an immutable local variable' },
    { label: 'mut', detail: 'Declare a mutable local variable' },
    { label: 'return', detail: 'Return from the current function' },
    { label: 'if', detail: 'Conditionally execute a block' },
    { label: 'else', detail: 'Alternative branch for an if statement' },
    { label: 'while', detail: 'Repeat a block while a condition is true' },
    { label: 'for', detail: 'Iterate over a range or collection' },
    { label: 'in', detail: 'Used with `for` to name the iterated source' },
    { label: 'struct', detail: 'Declare a struct type' },
    { label: 'enum', detail: 'Declare an enum type' },
    { label: 'use', detail: 'Import a module' },
    { label: 'pub', detail: 'Mark a declaration as public' },
    { label: 'as', detail: 'Alias an imported module (`use ... as name`)' },
    { label: 'true', detail: 'Boolean literal: true' },
    { label: 'false', detail: 'Boolean literal: false' },
];

/** Current primitive type spellings (semantic::TypeKind), excluding internal-only kinds and `str` (see file header). */
export const PRIMITIVE_TYPES: readonly CompletionMetadata[] = [
    { label: 'i8', detail: '8-bit signed integer' },
    { label: 'i16', detail: '16-bit signed integer' },
    { label: 'i32', detail: '32-bit signed integer' },
    { label: 'i64', detail: '64-bit signed integer' },
    { label: 'u8', detail: '8-bit unsigned integer' },
    { label: 'u16', detail: '16-bit unsigned integer' },
    { label: 'u32', detail: '32-bit unsigned integer' },
    { label: 'u64', detail: '64-bit unsigned integer' },
    { label: 'f32', detail: '32-bit floating-point number' },
    { label: 'f64', detail: '64-bit floating-point number' },
    { label: 'bool', detail: 'Boolean value: true or false' },
    { label: 'char', detail: 'Single character value' },
];

export interface BuiltinCompletionMetadata extends CompletionMetadata {
    readonly insertText: string;
}

/**
 * Only `print` - the ONE prelude builtin (SemanticAnalyzer.cpp's prelude
 * also has `panic`/`assert`) with real M6 native lowering. See this
 * file's own header comment for why the other two are omitted.
 */
export const BUILTIN_FUNCTIONS: readonly BuiltinCompletionMetadata[] = [
    {
        label: 'print',
        detail: 'Print a primitive value followed by a newline',
        insertText: 'print(${1:value})',
    },
];

/**
 * Bounded, lightweight lexical check (§9) - NOT a parser: true when the
 * text immediately before the cursor, ignoring trailing whitespace, ends
 * in `:` (a `let`/`mut`/parameter type annotation) or `->` (a function
 * return-type annotation). Used only to RANK primitive types higher in
 * that position - every completion item remains available regardless
 * (§9: "keep all primitive types available globally" is the fallback
 * this milestone intentionally leans on rather than a more complex
 * detector).
 */
export function isTypeAnnotationContext(textBeforeCursor: string): boolean {
    const trimmed = textBeforeCursor.replace(/[ \t]+$/, '');
    return trimmed.endsWith('->') || trimmed.endsWith(':');
}
