#include "kai/lexer/Token.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::Token;
using kai::TokenKind;

namespace {

void testDefaultTokenIsInvalid() {
    const Token token;

    KAI_CHECK(token.kind() == TokenKind::Invalid);
    KAI_CHECK(!token.span().isValid());
    KAI_CHECK(token.is(TokenKind::Invalid));
}

void testConstructionAndAccessors() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {}");

    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 2));
    const Token token(TokenKind::KwFn, span);

    KAI_CHECK(token.kind() == TokenKind::KwFn);
    KAI_CHECK(token.span() == span);
    KAI_CHECK(sm.text(token.span()) == "fn");
}

void testIs() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "+");
    const Token plus(TokenKind::Plus, SourceSpan(SourceLocation(file, 0), SourceLocation(file, 1)));

    KAI_CHECK(plus.is(TokenKind::Plus));
    KAI_CHECK(!plus.is(TokenKind::Minus));
}

void testEquality() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x");

    const SourceSpan letSpan(SourceLocation(file, 0), SourceLocation(file, 3));
    const SourceSpan xSpan(SourceLocation(file, 4), SourceLocation(file, 5));

    const Token a(TokenKind::KwLet, letSpan);
    const Token b(TokenKind::KwLet, letSpan);
    const Token c(TokenKind::Identifier, xSpan);
    const Token d(TokenKind::KwLet, xSpan); // same kind, different span

    KAI_CHECK(a == b);
    KAI_CHECK(!(a == c));
    KAI_CHECK(!(a == d));

    // Two default-constructed tokens compare equal.
    KAI_CHECK(Token() == Token());
}

} // namespace

int main() {
    testDefaultTokenIsInvalid();
    testConstructionAndAccessors();
    testIs();
    testEquality();

    return kai::test::failureCount == 0 ? 0 : 1;
}
