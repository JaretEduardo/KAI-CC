#pragma once

#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceLocation.hpp"

#include <cstdint>
#include <expected>
#include <optional>

namespace kai::parser {

/// The coarse reason a parse operation failed.
///
/// This distinguishes three genuinely different situations rather than
/// collapsing them into one generic "syntax error":
enum class ParseErrorKind : std::uint8_t {
    /// The token is lexically valid but is not valid grammar at this
    /// position (e.g. `fn 123() {}`).
    UnexpectedToken,

    /// The Lexer itself returned TokenKind::Invalid - a lexical failure,
    /// not a syntactic one. The Parser never tries to reinterpret it.
    InvalidToken,

    /// The token starts syntax that IS part of the current KAI grammar
    /// (GRAMMAR.md), but this parser milestone does not yet implement
    /// the corresponding AST node (e.g. `let x = 10`).
    UnsupportedSyntax,
};

/// A minimal, message-free description of a parse failure.
///
/// ParseError is a temporary stand-in for the future Diagnostic
/// subsystem: it carries only enough structured information (what kind
/// of failure, where, what token was actually seen, and - when a single
/// token was expected - what would have been accepted instead) to
/// support the current test suite and a later conversion into a real
/// diagnostic. It intentionally carries no rendered message, no
/// diagnostic code, no notes, and no recovery information, and has no
/// dependency on kai::ast.
struct ParseError {
    ParseErrorKind kind;
    SourceSpan span;
    TokenKind actual;

    /// Populated only when failure came from expecting exactly one
    /// token kind (see Parser::expect()). std::nullopt when no single
    /// token would have made the position valid (e.g. "expected an
    /// expression", which many different token kinds could start).
    std::optional<TokenKind> expected;
};

/// The result of one parser grammar rule: either the AST value it
/// builds, or the first ParseError encountered while building it.
template <typename T>
using ParseResult = std::expected<T, ParseError>;

} // namespace kai::parser
