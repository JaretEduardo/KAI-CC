#pragma once

#include "kai/lexer/Token.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceLocation.hpp"
#include "kai/source/SourceManager.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace kai {

/// Converts the source buffer for one file into a stream of Tokens.
///
/// Lexer recognizes lexical shape only: it never parses, never computes
/// literal values, and has no dependency on a future Diagnostic type.
/// Every call to nextToken() either returns EndOfFile (nothing left to
/// scan) or has advanced the cursor by at least one byte, so the Lexer
/// can never loop forever on malformed input.
///
/// Lexer does not own the SourceManager and does not duplicate the
/// source buffer; it caches a std::string_view into the buffer the
/// SourceManager already owns, which remains valid for the Lexer's
/// entire lifetime (SourceManager buffers have stable addresses once
/// registered).
class Lexer {
public:
    Lexer(const SourceManager& sources, FileId file);

    /// Produces the next token in the stream. Safe to call repeatedly
    /// past EndOfFile; every such call returns the same EndOfFile token.
    Token nextToken();

private:
    // --- cursor primitives ---
    bool isAtEnd() const noexcept;
    char peek() const noexcept;
    char peekNext() const noexcept;
    char advance() noexcept;
    bool match(char expected) noexcept;
    SourceLocation here() const noexcept;

    Token eofToken() const noexcept;

    /// Discards whitespace, line comments, and newlines suppressed by
    /// paren/bracket nesting. If a significant physical newline is
    /// reached (nesting depth zero), returns the Newline token for it
    /// directly. Otherwise returns std::nullopt once real content (or
    /// EOF) is reached, and the caller re-checks isAtEnd()/dispatches.
    std::optional<Token> skipTriviaAndMaybeNewline();

    // Each scanner assumes the identifying first byte is still
    // unconsumed and is responsible for consuming everything that
    // belongs to its token before returning.
    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString();
    Token scanChar();
    Token scanOperatorOrPunctuation();

    const SourceManager& sources_;
    FileId file_;
    std::string_view buffer_;
    std::uint32_t offset_ = 0;

    std::uint32_t parenDepth_ = 0;
    std::uint32_t bracketDepth_ = 0;
};

} // namespace kai
