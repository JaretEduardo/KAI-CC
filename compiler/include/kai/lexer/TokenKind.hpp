#pragma once

#include <cstdint>
#include <string_view>

namespace kai {

/// The lexical category of a Token.
///
/// This vocabulary covers only the current KAI 0.1 grammar (GRAMMAR.md).
/// Keywords or operators that are merely mentioned as future possibilities
/// must not be added here until they actually enter the supported grammar.
enum class TokenKind : std::uint8_t {
    // Special
    EndOfFile,
    Invalid,
    Newline,

    // Identifier
    Identifier,

    // Literals
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,

    // Keywords
    KwFn,
    KwLet,
    KwMut,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwStruct,
    KwEnum,
    KwUse,
    KwPub,
    KwAs,
    KwTrue,
    KwFalse,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    Equal,
    EqualEqual,
    BangEqual,

    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    AmpAmp,
    PipePipe,
    Bang,
    Amp,

    Arrow,    // ->
    DotDot,   // ..
    Question, // ?

    // Punctuation
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,

    Comma,
    Colon,
    Semicolon,
    Dot,
};

/// A stable, human-readable name for a TokenKind, matching its enumerator
/// spelling (e.g. TokenKind::KwFn -> "KwFn"). Intended for debugging and
/// future tooling (e.g. a `--tokens` dump); it performs no formatting of
/// its own beyond returning the name.
std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace kai
