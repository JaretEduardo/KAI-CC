#include "kai/lexer/TokenKind.hpp"

#include "support/check.hpp"

#include <array>

using kai::TokenKind;
using kai::tokenKindName;

namespace {

void testRepresentativeNames() {
    KAI_CHECK(tokenKindName(TokenKind::EndOfFile) == "EndOfFile");
    KAI_CHECK(tokenKindName(TokenKind::Invalid) == "Invalid");
    KAI_CHECK(tokenKindName(TokenKind::Newline) == "Newline");
    KAI_CHECK(tokenKindName(TokenKind::Identifier) == "Identifier");
    KAI_CHECK(tokenKindName(TokenKind::IntegerLiteral) == "IntegerLiteral");
    KAI_CHECK(tokenKindName(TokenKind::KwFn) == "KwFn");
    KAI_CHECK(tokenKindName(TokenKind::Plus) == "Plus");
    KAI_CHECK(tokenKindName(TokenKind::LeftParen) == "LeftParen");
}

void testKwAsIsRepresented() {
    KAI_CHECK(tokenKindName(TokenKind::KwAs) == "KwAs");
}

void testNewlineIsRepresented() {
    KAI_CHECK(tokenKindName(TokenKind::Newline) == "Newline");
}

// Every TokenKind enumerator, kept in sync by hand since C++ has no
// reflection. If a new enumerator is added without updating this list,
// this test will not catch it directly, but the non-`default:` switch in
// TokenKind.cpp will produce a -Wswitch build warning instead.
void testEveryEnumeratorHasANonEmptyName() {
    static constexpr std::array kAllKinds{
        TokenKind::EndOfFile,
        TokenKind::Invalid,
        TokenKind::Newline,

        TokenKind::Identifier,

        TokenKind::IntegerLiteral,
        TokenKind::FloatLiteral,
        TokenKind::StringLiteral,
        TokenKind::CharLiteral,

        TokenKind::KwFn,
        TokenKind::KwLet,
        TokenKind::KwMut,
        TokenKind::KwReturn,
        TokenKind::KwIf,
        TokenKind::KwElse,
        TokenKind::KwWhile,
        TokenKind::KwFor,
        TokenKind::KwIn,
        TokenKind::KwStruct,
        TokenKind::KwEnum,
        TokenKind::KwUse,
        TokenKind::KwPub,
        TokenKind::KwAs,
        TokenKind::KwTrue,
        TokenKind::KwFalse,

        TokenKind::Plus,
        TokenKind::Minus,
        TokenKind::Star,
        TokenKind::Slash,
        TokenKind::Percent,

        TokenKind::Equal,
        TokenKind::EqualEqual,
        TokenKind::BangEqual,

        TokenKind::Less,
        TokenKind::LessEqual,
        TokenKind::Greater,
        TokenKind::GreaterEqual,

        TokenKind::AmpAmp,
        TokenKind::PipePipe,
        TokenKind::Bang,
        TokenKind::Amp,

        TokenKind::Arrow,
        TokenKind::DotDot,
        TokenKind::Question,

        TokenKind::LeftParen,
        TokenKind::RightParen,
        TokenKind::LeftBrace,
        TokenKind::RightBrace,
        TokenKind::LeftBracket,
        TokenKind::RightBracket,

        TokenKind::Comma,
        TokenKind::Colon,
        TokenKind::Semicolon,
        TokenKind::Dot,
    };

    for (const TokenKind kind : kAllKinds) {
        KAI_CHECK(!tokenKindName(kind).empty());
    }
}

} // namespace

int main() {
    testRepresentativeNames();
    testKwAsIsRepresented();
    testNewlineIsRepresented();
    testEveryEnumeratorHasANonEmptyName();

    return kai::test::failureCount == 0 ? 0 : 1;
}
