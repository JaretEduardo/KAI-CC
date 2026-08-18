#include "kai/lexer/Lexer.hpp"
#include "kai/lexer/Token.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using kai::FileId;
using kai::Lexer;
using kai::SourceManager;
using kai::Token;
using kai::TokenKind;

namespace {

std::vector<Token> lexAll(SourceManager& sm, std::string_view source) {
    const FileId file = sm.addVirtualFile("test.kai", std::string(source));
    Lexer lexer(sm, file);

    std::vector<Token> tokens;
    while (true) {
        const Token tok = lexer.nextToken();
        tokens.push_back(tok);
        if (tok.is(TokenKind::EndOfFile)) {
            break;
        }
    }
    return tokens;
}

std::vector<TokenKind> kindsOf(const std::vector<Token>& tokens) {
    std::vector<TokenKind> kinds;
    kinds.reserve(tokens.size());
    for (const Token& t : tokens) {
        kinds.push_back(t.kind());
    }
    return kinds;
}

// ---------------------------------------------------------------------
// Identifiers / keywords
// ---------------------------------------------------------------------

void testIdentifiers() {
    SourceManager sm;
    for (const std::string_view name : {"value", "load_user", "Buffer", "x2", "_private"}) {
        const auto tokens = lexAll(sm, name);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Identifier, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == name);
    }
}

void testAllKeywords() {
    static constexpr std::pair<std::string_view, TokenKind> kKeywords[] = {
        {"fn", TokenKind::KwFn},         {"let", TokenKind::KwLet},     {"mut", TokenKind::KwMut},
        {"return", TokenKind::KwReturn}, {"if", TokenKind::KwIf},       {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile},   {"for", TokenKind::KwFor},     {"in", TokenKind::KwIn},
        {"struct", TokenKind::KwStruct}, {"enum", TokenKind::KwEnum},   {"use", TokenKind::KwUse},
        {"pub", TokenKind::KwPub},       {"as", TokenKind::KwAs},       {"true", TokenKind::KwTrue},
        {"false", TokenKind::KwFalse},
    };

    SourceManager sm;
    for (const auto& [text, kind] : kKeywords) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(tokens.size() == 2);
        KAI_CHECK(tokens[0].kind() == kind);
        KAI_CHECK(tokens[1].kind() == TokenKind::EndOfFile);
    }
}

void testKeywordPrefixedIdentifiersStayIdentifiers() {
    SourceManager sm;
    for (const std::string_view name : {"fnx", "let1", "structure"}) {
        const auto tokens = lexAll(sm, name);
        KAI_CHECK(tokens[0].kind() == TokenKind::Identifier);
    }
}

// ---------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------

void testIntegerLiterals() {
    SourceManager sm;
    for (const std::string_view text : {"0", "42", "123456"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::IntegerLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testFloatLiterals() {
    SourceManager sm;
    for (const std::string_view text : {"0.0", "1.5", "3.14", "42.5"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::FloatLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testDotDotIsNotAFloat() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "1..10");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::IntegerLiteral,
                                     TokenKind::DotDot,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::EndOfFile,
                                 }));
}

void testDotFollowedByIdentifierIsNotAFloat() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "1.foo");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Dot,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
}

void testTrailingDotIsNotAFloat() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "3.");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Dot,
                                     TokenKind::EndOfFile,
                                 }));
}

void testLeadingDotIsNotAFloat() {
    SourceManager sm;
    const auto tokens = lexAll(sm, ".5");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Dot,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::EndOfFile,
                                 }));
}

void testUnsupportedNumericFormsAreOneInvalidToken() {
    SourceManager sm;
    for (const std::string_view text : {"1e10", "1e-10", "42u64", "42i64", "1_000", "3.14f32"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

// '+'/'-' must only be absorbed while recovering an unsupported
// numeric-like sequence (e.g. an exponent sign), never when they are
// ordinary arithmetic operators immediately following a plain integer.
void testArithmeticSignsAreNotAbsorbedByPlainIntegers() {
    SourceManager sm;

    {
        const auto tokens = lexAll(sm, "1-10");
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                         TokenKind::IntegerLiteral,
                                         TokenKind::Minus,
                                         TokenKind::IntegerLiteral,
                                         TokenKind::EndOfFile,
                                     }));
        KAI_CHECK(sm.text(tokens[0].span()) == "1");
        KAI_CHECK(sm.text(tokens[1].span()) == "-");
        KAI_CHECK(sm.text(tokens[2].span()) == "10");
    }

    {
        const auto tokens = lexAll(sm, "1+10");
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                         TokenKind::IntegerLiteral,
                                         TokenKind::Plus,
                                         TokenKind::IntegerLiteral,
                                         TokenKind::EndOfFile,
                                     }));
        KAI_CHECK(sm.text(tokens[0].span()) == "1");
        KAI_CHECK(sm.text(tokens[1].span()) == "+");
        KAI_CHECK(sm.text(tokens[2].span()) == "10");
    }
}

// The same sign, when it follows an exponent-shaped letter directly
// attached to a number, is part of the malformed sequence and must stay
// absorbed into a single Invalid token.
void testExponentSignsStayAbsorbedIntoInvalidToken() {
    SourceManager sm;

    {
        const auto tokens = lexAll(sm, "1e-10");
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == "1e-10");
    }

    {
        const auto tokens = lexAll(sm, "1e+10");
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == "1e+10");
    }
}

// ---------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------

void testValidStrings() {
    SourceManager sm;
    for (const std::string_view text : {"\"Hello\"", "\"KAI\""}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::StringLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testSupportedStringEscapes() {
    SourceManager sm;
    for (const std::string_view text : {R"("Hello\nKAI")", R"("quoted: \"KAI\"")", R"("\r\t\\\0")"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::StringLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testUnsupportedStringEscapeRecovery() {
    SourceManager sm;
    // A single malformed escape must produce ONE Invalid token covering
    // the whole literal, not several unrelated tokens.
    for (const std::string_view text : {R"("\x41")", R"("\u1234")", R"("hello\qworld")"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testUnterminatedStringAtEof() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "\"hello");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
    KAI_CHECK(sm.text(tokens[0].span()) == "\"hello");
}

void testUnterminatedStringAtNewline() {
    SourceManager sm;
    // A raw newline inside "..." terminates the literal as Invalid, but
    // the newline byte itself is left for normal newline handling.
    const std::string source = "\"hello\nworld\"";
    const auto tokens = lexAll(sm, source);
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Invalid,   // "hello
                                     TokenKind::Newline,   // the raw \n, processed normally
                                     TokenKind::Identifier, // world
                                     TokenKind::Invalid,    // the orphaned closing quote, unterminated at EOF
                                     TokenKind::EndOfFile,
                                 }));
    KAI_CHECK(sm.text(tokens[0].span()) == "\"hello");
}

// ---------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------

void testValidChars() {
    SourceManager sm;
    for (const std::string_view text : {"'A'", "'0'"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::CharLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testSupportedCharEscapes() {
    SourceManager sm;
    for (const std::string_view text : {R"('\n')", R"('\r')", R"('\t')", R"('\\')", R"('\'')", R"('\0')"}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::CharLiteral, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testInvalidCharRecovery() {
    SourceManager sm;
    // Each of these must produce exactly one Invalid token covering the
    // whole malformed literal, not several unrelated tokens.
    for (const std::string_view text : {"''", "'ab'", R"('\q')", "'\xC3\xA1'" /* 'á' */}) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testUnterminatedCharAtEof() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "'a");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
    KAI_CHECK(sm.text(tokens[0].span()) == "'a");
}

// ---------------------------------------------------------------------
// Operators / punctuation
// ---------------------------------------------------------------------

void testLongestMatchOperators() {
    SourceManager sm;
    static constexpr std::pair<std::string_view, TokenKind> kCases[] = {
        {"=", TokenKind::Equal},      {"==", TokenKind::EqualEqual}, {"!", TokenKind::Bang},
        {"!=", TokenKind::BangEqual}, {"<", TokenKind::Less},        {"<=", TokenKind::LessEqual},
        {">", TokenKind::Greater},    {">=", TokenKind::GreaterEqual}, {"&", TokenKind::Amp},
        {"&&", TokenKind::AmpAmp},    {"-", TokenKind::Minus},       {"->", TokenKind::Arrow},
        {".", TokenKind::Dot},        {"..", TokenKind::DotDot},     {"+", TokenKind::Plus},
        {"*", TokenKind::Star},       {"/", TokenKind::Slash},       {"%", TokenKind::Percent},
        {"?", TokenKind::Question},   {"||", TokenKind::PipePipe},
    };
    for (const auto& [text, kind] : kCases) {
        const auto tokens = lexAll(sm, text);
        KAI_CHECK(tokens.size() == 2);
        KAI_CHECK(tokens[0].kind() == kind);
        KAI_CHECK(sm.text(tokens[0].span()) == text);
    }
}

void testStandalonePipeIsInvalid() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "|");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::Invalid, TokenKind::EndOfFile}));
}

void testPunctuation() {
    SourceManager sm;
    static constexpr std::pair<char, TokenKind> kCases[] = {
        {'(', TokenKind::LeftParen},   {')', TokenKind::RightParen}, {'{', TokenKind::LeftBrace},
        {'}', TokenKind::RightBrace},  {'[', TokenKind::LeftBracket}, {']', TokenKind::RightBracket},
        {',', TokenKind::Comma},       {':', TokenKind::Colon},      {';', TokenKind::Semicolon},
    };
    for (const auto& [ch, kind] : kCases) {
        const auto tokens = lexAll(sm, std::string_view(&ch, 1));
        KAI_CHECK(tokens.size() == 2);
        KAI_CHECK(tokens[0].kind() == kind);
    }
}

void testUnknownCharacterRecovers() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "@ x");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Invalid,     // '@'
                                     TokenKind::Identifier,  // x
                                     TokenKind::EndOfFile,
                                 }));
    KAI_CHECK(sm.text(tokens[0].span()) == "@");
}

// ---------------------------------------------------------------------
// Comments / whitespace / newlines
// ---------------------------------------------------------------------

void testLineCommentAlone() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "// comment");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({TokenKind::EndOfFile}));
}

void testCommentPreservesFollowingNewline() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "let x = 10 // comment\nprint(x)");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::KwLet,
                                     TokenKind::Identifier,
                                     TokenKind::Equal,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::LeftParen,
                                     TokenKind::Identifier,
                                     TokenKind::RightParen,
                                     TokenKind::EndOfFile,
                                 }));
}

void testNewlineLf() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "a\nb");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Identifier,
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
    KAI_CHECK(sm.text(tokens[1].span()) == "\n");
}

void testNewlineCrLf() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "a\r\nb");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Identifier,
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
    KAI_CHECK(sm.text(tokens[1].span()) == "\r\n");
}

void testStandaloneCrIsWhitespace() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "a\rb");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Identifier,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
}

void testCommentThenCrLf() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "// comment\r\nx");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
    KAI_CHECK(sm.text(tokens[0].span()) == "\r\n");
}

void testStandaloneCrInsideCommentDoesNotTerminateIt() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "// comment\rstill comment\nx");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::EndOfFile,
                                 }));
}

void testNewlineSuppressedInParens() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "add(\n    10,\n    20\n)");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Identifier,
                                     TokenKind::LeftParen,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Comma,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::RightParen,
                                     TokenKind::EndOfFile,
                                 }));
}

void testNewlineSuppressedInBrackets() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "[1,\n2,\n3]");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::LeftBracket,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Comma,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Comma,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::RightBracket,
                                     TokenKind::EndOfFile,
                                 }));
}

void testNewlinePreservedInBraces() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "{\nlet x = 10\nprint(x)\n}");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::LeftBrace,
                                     TokenKind::Newline,
                                     TokenKind::KwLet,
                                     TokenKind::Identifier,
                                     TokenKind::Equal,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Newline,
                                     TokenKind::Identifier,
                                     TokenKind::LeftParen,
                                     TokenKind::Identifier,
                                     TokenKind::RightParen,
                                     TokenKind::Newline,
                                     TokenKind::RightBrace,
                                     TokenKind::EndOfFile,
                                 }));
}

void testNestedDelimiters() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "f((1,\n[2, 3]))");
    KAI_CHECK(kindsOf(tokens) == std::vector<TokenKind>({
                                     TokenKind::Identifier,
                                     TokenKind::LeftParen,
                                     TokenKind::LeftParen,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Comma,
                                     TokenKind::LeftBracket,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::Comma,
                                     TokenKind::IntegerLiteral,
                                     TokenKind::RightBracket,
                                     TokenKind::RightParen,
                                     TokenKind::RightParen,
                                     TokenKind::EndOfFile,
                                 }));
}

void testUnmatchedClosingDelimitersDoNotCorruptDepth() {
    SourceManager sm;

    const auto strayTokens = lexAll(sm, ")]");
    KAI_CHECK(kindsOf(strayTokens) == std::vector<TokenKind>({
                                           TokenKind::RightParen,
                                           TokenKind::RightBracket,
                                           TokenKind::EndOfFile,
                                       }));

    // If the depth counters had underflowed instead of clamping at zero,
    // this newline would be wrongly suppressed.
    const auto followedByNewline = lexAll(sm, ")\na");
    KAI_CHECK(kindsOf(followedByNewline) == std::vector<TokenKind>({
                                                 TokenKind::RightParen,
                                                 TokenKind::Newline,
                                                 TokenKind::Identifier,
                                                 TokenKind::EndOfFile,
                                             }));
}

// ---------------------------------------------------------------------
// EOF
// ---------------------------------------------------------------------

void testEofSpanIsAtBufferEnd() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abc");
    Lexer lexer(sm, file);

    Token tok;
    do {
        tok = lexer.nextToken();
    } while (!tok.is(TokenKind::EndOfFile));

    KAI_CHECK(tok.span().isValid());
    KAI_CHECK(tok.span().begin() == tok.span().end());
    KAI_CHECK(tok.span().begin().offset() == 3);
}

void testRepeatedEofCallsAreIdempotent() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "x");
    Lexer lexer(sm, file);

    (void)lexer.nextToken(); // Identifier x
    const Token first = lexer.nextToken();
    const Token second = lexer.nextToken();
    const Token third = lexer.nextToken();

    KAI_CHECK(first.is(TokenKind::EndOfFile));
    KAI_CHECK(first == second);
    KAI_CHECK(second == third);
}

// ---------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------

void testAccurateSpansForRepresentativeTokens() {
    SourceManager sm;
    const auto tokens = lexAll(sm, "fn main() {}");

    KAI_CHECK(sm.text(tokens[0].span()) == "fn");
    KAI_CHECK(sm.text(tokens[1].span()) == "main");
    KAI_CHECK(sm.text(tokens[2].span()) == "(");
    KAI_CHECK(sm.text(tokens[3].span()) == ")");
    KAI_CHECK(sm.text(tokens[4].span()) == "{");
    KAI_CHECK(sm.text(tokens[5].span()) == "}");
}

} // namespace

int main() {
    testIdentifiers();
    testAllKeywords();
    testKeywordPrefixedIdentifiersStayIdentifiers();

    testIntegerLiterals();
    testFloatLiterals();
    testDotDotIsNotAFloat();
    testDotFollowedByIdentifierIsNotAFloat();
    testTrailingDotIsNotAFloat();
    testLeadingDotIsNotAFloat();
    testUnsupportedNumericFormsAreOneInvalidToken();
    testArithmeticSignsAreNotAbsorbedByPlainIntegers();
    testExponentSignsStayAbsorbedIntoInvalidToken();

    testValidStrings();
    testSupportedStringEscapes();
    testUnsupportedStringEscapeRecovery();
    testUnterminatedStringAtEof();
    testUnterminatedStringAtNewline();

    testValidChars();
    testSupportedCharEscapes();
    testInvalidCharRecovery();
    testUnterminatedCharAtEof();

    testLongestMatchOperators();
    testStandalonePipeIsInvalid();
    testPunctuation();
    testUnknownCharacterRecovers();

    testLineCommentAlone();
    testCommentPreservesFollowingNewline();
    testNewlineLf();
    testNewlineCrLf();
    testStandaloneCrIsWhitespace();
    testCommentThenCrLf();
    testStandaloneCrInsideCommentDoesNotTerminateIt();
    testNewlineSuppressedInParens();
    testNewlineSuppressedInBrackets();
    testNewlinePreservedInBraces();
    testNestedDelimiters();
    testUnmatchedClosingDelimitersDoNotCorruptDepth();

    testEofSpanIsAtBufferEnd();
    testRepeatedEofCallsAreIdempotent();

    testAccurateSpansForRepresentativeTokens();

    return kai::test::failureCount == 0 ? 0 : 1;
}
