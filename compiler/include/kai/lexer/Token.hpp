#pragma once

#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceLocation.hpp"

namespace kai {

/// A single lexical unit produced by the (future) Lexer.
///
/// Token intentionally stores nothing but its kind and its source span.
/// The lexeme text is never cached here; it is always recoverable through
/// SourceManager::text(token.span()). Literal tokens (IntegerLiteral,
/// FloatLiteral, StringLiteral, CharLiteral) likewise carry no parsed or
/// decoded value: the lexer only determines that source text has the
/// lexical shape of a literal, and value conversion is deferred to a
/// later frontend component once the lexical grammar for numbers and
/// escapes is fully defined. This keeps Token stable across that future
/// change.
class Token {
public:
    /// An unset/invalid token: kind() == TokenKind::Invalid, span() invalid.
    constexpr Token() noexcept = default;

    constexpr Token(TokenKind kind, SourceSpan span) noexcept : kind_(kind), span_(span) {}

    constexpr TokenKind kind() const noexcept { return kind_; }
    constexpr SourceSpan span() const noexcept { return span_; }

    constexpr bool is(TokenKind kind) const noexcept { return kind_ == kind; }

    friend constexpr bool operator==(Token lhs, Token rhs) noexcept = default;

private:
    TokenKind kind_ = TokenKind::Invalid;
    SourceSpan span_{};
};

} // namespace kai
