#include "kai/lexer/Lexer.hpp"

#include <cassert>
#include <utility>

namespace kai {

namespace {

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool isIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isIdentifierContinue(char c) noexcept { return isIdentifierStart(c) || isDigit(c); }

// Strings and character literals support slightly different escape sets
// (strings escape `"`, chars escape `'`); keep them distinct rather than
// silently accepting the union of both.
bool isSupportedStringEscapeChar(char c) noexcept {
    switch (c) {
        case 'n':
        case 'r':
        case 't':
        case '\\':
        case '"':
        case '0':
            return true;
        default:
            return false;
    }
}

bool isSupportedCharEscapeChar(char c) noexcept {
    switch (c) {
        case 'n':
        case 'r':
        case 't':
        case '\\':
        case '\'':
        case '0':
            return true;
        default:
            return false;
    }
}

} // namespace

Lexer::Lexer(const SourceManager& sources, FileId file) : file_(file), buffer_(sources.buffer(file)) {}

bool Lexer::isAtEnd() const noexcept { return offset_ >= buffer_.size(); }

char Lexer::peek() const noexcept { return isAtEnd() ? '\0' : buffer_[offset_]; }

char Lexer::peekNext() const noexcept {
    return (offset_ + 1 >= buffer_.size()) ? '\0' : buffer_[offset_ + 1];
}

char Lexer::advance() noexcept {
    assert(!isAtEnd());
    return buffer_[offset_++];
}

bool Lexer::match(char expected) noexcept {
    if (peek() != expected) {
        return false;
    }
    advance();
    return true;
}

SourceLocation Lexer::here() const noexcept { return SourceLocation(file_, offset_); }

Token Lexer::eofToken() const noexcept {
    return Token(TokenKind::EndOfFile,
                 SourceSpan::point(SourceLocation(file_, static_cast<std::uint32_t>(buffer_.size()))));
}

std::optional<Token> Lexer::skipTriviaAndMaybeNewline() {
    while (!isAtEnd()) {
        const char c = peek();

        if (c == ' ' || c == '\t') {
            advance();
            continue;
        }

        if (c == '\r' && peekNext() == '\n') {
            const SourceLocation start = here();
            advance(); // '\r'
            advance(); // '\n'
            if (parenDepth_ == 0 && bracketDepth_ == 0) {
                return Token(TokenKind::Newline, SourceSpan(start, here()));
            }
            continue; // suppressed inside (...) or [...]
        }

        if (c == '\n') {
            const SourceLocation start = here();
            advance();
            if (parenDepth_ == 0 && bracketDepth_ == 0) {
                return Token(TokenKind::Newline, SourceSpan(start, here()));
            }
            continue; // suppressed inside (...) or [...]
        }

        if (c == '\r') {
            // Standalone '\r' (not part of a CRLF pair) is whitespace,
            // not a KAI physical newline.
            advance();
            continue;
        }

        if (c == '/' && peekNext() == '/') {
            advance(); // first '/'
            advance(); // second '/'
            // Runs until (but not including) '\n', or a '\r' that starts
            // a CRLF pair, or EOF. A standalone '\r' does not terminate
            // the comment.
            while (!isAtEnd()) {
                const char cc = peek();
                if (cc == '\n') {
                    break;
                }
                if (cc == '\r' && peekNext() == '\n') {
                    break;
                }
                advance();
            }
            continue; // let the next loop iteration handle the newline/EOF
        }

        break; // real content
    }

    return std::nullopt;
}

Token Lexer::scanIdentifierOrKeyword() {
    static constexpr std::pair<std::string_view, TokenKind> kKeywords[] = {
        {"fn", TokenKind::KwFn},         {"let", TokenKind::KwLet},     {"mut", TokenKind::KwMut},
        {"return", TokenKind::KwReturn}, {"if", TokenKind::KwIf},       {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile},   {"for", TokenKind::KwFor},     {"in", TokenKind::KwIn},
        {"struct", TokenKind::KwStruct}, {"enum", TokenKind::KwEnum},   {"use", TokenKind::KwUse},
        {"pub", TokenKind::KwPub},       {"as", TokenKind::KwAs},       {"true", TokenKind::KwTrue},
        {"false", TokenKind::KwFalse},
    };

    const SourceLocation start = here();
    const std::uint32_t startOffset = offset_;

    advance(); // identifier-start byte, already verified by the caller
    while (isIdentifierContinue(peek())) {
        advance();
    }

    const std::string_view lexeme = buffer_.substr(startOffset, offset_ - startOffset);

    for (const auto& [text, kind] : kKeywords) {
        if (lexeme == text) {
            return Token(kind, SourceSpan(start, here()));
        }
    }

    return Token(TokenKind::Identifier, SourceSpan(start, here()));
}

Token Lexer::scanNumber() {
    const SourceLocation start = here();

    while (isDigit(peek())) {
        advance();
    }

    bool isFloat = false;
    if (peek() == '.' && isDigit(peekNext())) {
        isFloat = true;
        advance(); // '.'
        while (isDigit(peek())) {
            advance();
        }
    }

    // An identifier-shaped sequence glued directly onto a number (an
    // exponent, a typed suffix, a digit separator, ...) is not part of
    // the KAI 0.1 numeric grammar. Consume the whole malformed run and
    // report it as a single Invalid token rather than letting it split
    // into unrelated tokens. A '+'/'-' immediately following such a run
    // is also consumed when followed by a digit, so exponent-shaped
    // input like "1e-10" stays one Invalid token instead of splitting
    // into "1e" Minus "10".
    if (isIdentifierStart(peek())) {
        while (isIdentifierContinue(peek()) || ((peek() == '+' || peek() == '-') && isDigit(peekNext()))) {
            advance();
        }
        return Token(TokenKind::Invalid, SourceSpan(start, here()));
    }

    return Token(isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, SourceSpan(start, here()));
}

Token Lexer::scanString() {
    const SourceLocation start = here();
    advance(); // opening '"'

    bool valid = true;

    while (true) {
        if (isAtEnd()) {
            valid = false;
            break;
        }

        const char c = peek();

        if (c == '\n' || c == '\r') {
            // A raw newline is not permitted inside an ordinary string;
            // stop without consuming it so normal newline handling
            // processes it (and a full CRLF pair) intact afterward.
            valid = false;
            break;
        }

        if (c == '"') {
            advance(); // closing quote
            break;
        }

        if (c == '\\') {
            advance(); // backslash
            if (isAtEnd()) {
                valid = false;
                break;
            }
            const char escaped = advance();
            if (!isSupportedStringEscapeChar(escaped)) {
                valid = false;
                // Keep scanning: recover to the closing quote (or
                // newline/EOF) instead of stopping at the first bad
                // escape, so one malformed literal produces one
                // Invalid token instead of several unrelated ones.
            }
            continue;
        }

        advance(); // ordinary byte
    }

    return Token(valid ? TokenKind::StringLiteral : TokenKind::Invalid, SourceSpan(start, here()));
}

Token Lexer::scanChar() {
    const SourceLocation start = here();
    advance(); // opening '\''

    bool valid = true;
    int contentUnits = 0;

    while (true) {
        if (isAtEnd()) {
            valid = false;
            break;
        }

        const char c = peek();

        if (c == '\n' || c == '\r') {
            valid = false;
            break;
        }

        if (c == '\'') {
            advance(); // closing quote
            break;
        }

        if (c == '\\') {
            advance(); // backslash
            if (isAtEnd()) {
                valid = false;
                break;
            }
            const char escaped = advance();
            if (!isSupportedCharEscapeChar(escaped)) {
                valid = false;
            }
            ++contentUnits;
            continue;
        }

        advance(); // ordinary byte
        ++contentUnits;
    }

    if (contentUnits != 1) {
        valid = false;
    }

    return Token(valid ? TokenKind::CharLiteral : TokenKind::Invalid, SourceSpan(start, here()));
}

Token Lexer::scanOperatorOrPunctuation() {
    const SourceLocation start = here();
    const char c = advance();

    TokenKind kind;
    switch (c) {
        case '+':
            kind = TokenKind::Plus;
            break;
        case '-':
            kind = match('>') ? TokenKind::Arrow : TokenKind::Minus;
            break;
        case '*':
            kind = TokenKind::Star;
            break;
        case '/':
            kind = TokenKind::Slash;
            break;
        case '%':
            kind = TokenKind::Percent;
            break;
        case '=':
            kind = match('=') ? TokenKind::EqualEqual : TokenKind::Equal;
            break;
        case '!':
            kind = match('=') ? TokenKind::BangEqual : TokenKind::Bang;
            break;
        case '<':
            kind = match('=') ? TokenKind::LessEqual : TokenKind::Less;
            break;
        case '>':
            kind = match('=') ? TokenKind::GreaterEqual : TokenKind::Greater;
            break;
        case '&':
            kind = match('&') ? TokenKind::AmpAmp : TokenKind::Amp;
            break;
        case '|':
            // A standalone '|' is not part of the KAI 0.1 operator
            // vocabulary; only '||' is supported.
            kind = match('|') ? TokenKind::PipePipe : TokenKind::Invalid;
            break;
        case '.':
            kind = match('.') ? TokenKind::DotDot : TokenKind::Dot;
            break;
        case '?':
            kind = TokenKind::Question;
            break;
        case '(':
            ++parenDepth_;
            kind = TokenKind::LeftParen;
            break;
        case ')':
            if (parenDepth_ > 0) {
                --parenDepth_;
            }
            kind = TokenKind::RightParen;
            break;
        case '{':
            kind = TokenKind::LeftBrace;
            break;
        case '}':
            kind = TokenKind::RightBrace;
            break;
        case '[':
            ++bracketDepth_;
            kind = TokenKind::LeftBracket;
            break;
        case ']':
            if (bracketDepth_ > 0) {
                --bracketDepth_;
            }
            kind = TokenKind::RightBracket;
            break;
        case ',':
            kind = TokenKind::Comma;
            break;
        case ':':
            kind = TokenKind::Colon;
            break;
        case ';':
            kind = TokenKind::Semicolon;
            break;
        default:
            kind = TokenKind::Invalid;
            break;
    }

    return Token(kind, SourceSpan(start, here()));
}

Token Lexer::nextToken() {
    if (isAtEnd()) {
        return eofToken();
    }

    if (std::optional<Token> newline = skipTriviaAndMaybeNewline()) {
        return *newline;
    }

    if (isAtEnd()) {
        return eofToken();
    }

    const char c = peek();

    if (isIdentifierStart(c)) {
        return scanIdentifierOrKeyword();
    }
    if (isDigit(c)) {
        return scanNumber();
    }
    if (c == '"') {
        return scanString();
    }
    if (c == '\'') {
        return scanChar();
    }

    return scanOperatorOrPunctuation();
}

} // namespace kai
