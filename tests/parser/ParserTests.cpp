#include "kai/parser/Parser.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceManager;
using kai::TokenKind;
using kai::ast::CallExpr;
using kai::ast::DeclKind;
using kai::ast::ExprKind;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::ParenExpr;
using kai::ast::SourceFile;
using kai::ast::StmtKind;
using kai::ast::TypeSyntaxKind;
using kai::parser::ParseError;
using kai::parser::ParseErrorKind;
using kai::parser::Parser;

namespace {

// --- Hello World: full shape ---

void testHelloWorldParsesFullShape() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("hello.kai", "fn main() {\n    print(\"Hello from KAI\")\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }

    const SourceFile& sourceFile = *result;
    KAI_CHECK(sourceFile.declarations().size() == 1);
    if (sourceFile.declarations().size() != 1) {
        return;
    }

    KAI_CHECK(sourceFile.declarations()[0]->kind() == DeclKind::Function);
    const auto& fn = static_cast<const FunctionDecl&>(*sourceFile.declarations()[0]);

    KAI_CHECK(!fn.isPublic());
    KAI_CHECK(sm.text(fn.name().span) == "main");
    KAI_CHECK(fn.params().empty());
    KAI_CHECK(fn.returnType() == nullptr);

    KAI_CHECK(fn.body().statements().size() == 1);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::Expr);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Call);
    const auto& call = static_cast<const CallExpr&>(exprStmt.expr());

    KAI_CHECK(call.callee().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(call.callee().span()) == "print");

    KAI_CHECK(call.arguments().size() == 1);
    KAI_CHECK(call.arguments()[0]->kind() == ExprKind::Literal);
    const auto& literal = static_cast<const LiteralExpr&>(*call.arguments()[0]);
    KAI_CHECK(literal.literalKind() == LiteralKind::String);
    KAI_CHECK(sm.text(literal.span()) == "\"Hello from KAI\"");

    // Full-span sanity checks.
    KAI_CHECK(sm.text(sourceFile.declarations()[0]->span()).starts_with("fn main()"));
    KAI_CHECK(sm.text(fn.body().span()).starts_with("{"));
    KAI_CHECK(sm.text(call.span()) == "print(\"Hello from KAI\")");
}

// --- Function signature variants ---

void testPublicFunctionRecordsVisibility() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "pub fn main() {}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.isPublic());
    KAI_CHECK(sm.text(fn.name().span) == "main");
}

void testFunctionWithParamsAndReturnType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn add(a: i32, b: i32) -> i32 {\n    print(a)\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.params().size() == 2);

    KAI_CHECK(sm.text(fn.params()[0].name.span) == "a");
    KAI_CHECK(fn.params()[0].type->kind() == TypeSyntaxKind::Named);
    KAI_CHECK(sm.text(fn.params()[0].type->span()) == "i32");

    KAI_CHECK(sm.text(fn.params()[1].name.span) == "b");
    KAI_CHECK(sm.text(fn.params()[1].type->span()) == "i32");

    KAI_CHECK(fn.returnType() != nullptr);
    KAI_CHECK(fn.returnType()->kind() == TypeSyntaxKind::Named);
    KAI_CHECK(sm.text(fn.returnType()->span()) == "i32");
}

// --- Block shapes ---

void testEmptyBlock() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().empty());
}

void testBlockWithBlankLineOnly() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().empty());
}

void testSemicolonSeparatedStatements() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\");\n    print(\"b\");\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().size() == 2);
}

void testMixedNewlineAndSemicolonSeparators() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\")\n    print(\"b\");\n    print(\"c\")\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().size() == 3);
}

// --- Expressions ---

void testParenthesizedExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print((42))\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(exprStmt.expr());

    KAI_CHECK(call.arguments().size() == 1);
    KAI_CHECK(call.arguments()[0]->kind() == ExprKind::Paren);
    const auto& paren = static_cast<const ParenExpr&>(*call.arguments()[0]);

    KAI_CHECK(sm.text(paren.span()) == "(42)");
    KAI_CHECK(paren.inner().kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(paren.inner().span()) == "42");
    KAI_CHECK(paren.span() != paren.inner().span());
}

void testZeroArgumentCall() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print()\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(exprStmt.expr());
    KAI_CHECK(call.arguments().empty());
}

void testMultipleArguments() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo(1, 2, 3)\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(exprStmt.expr());

    KAI_CHECK(call.arguments().size() == 3);
    KAI_CHECK(sm.text(call.arguments()[0]->span()) == "1");
    KAI_CHECK(sm.text(call.arguments()[1]->span()) == "2");
    KAI_CHECK(sm.text(call.arguments()[2]->span()) == "3");
}

void testChainedCalls() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo()()\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Call);
    const auto& outerCall = static_cast<const CallExpr&>(exprStmt.expr());
    KAI_CHECK(outerCall.arguments().empty());

    KAI_CHECK(outerCall.callee().kind() == ExprKind::Call);
    const auto& innerCall = static_cast<const CallExpr&>(outerCall.callee());
    KAI_CHECK(innerCall.arguments().empty());
    KAI_CHECK(innerCall.callee().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(innerCall.callee().span()) == "foo");
}

void testAllSupportedLiteralForms() {
    SourceManager sm;
    const FileId file =
        sm.addVirtualFile("a.kai", "fn main() {\n    42\n    3.14\n    \"s\"\n    'c'\n    true\n    false\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().size() == 6);

    const LiteralKind expectedKinds[] = {LiteralKind::Integer, LiteralKind::Float,  LiteralKind::String,
                                          LiteralKind::Char,    LiteralKind::Bool,   LiteralKind::Bool};

    for (std::size_t i = 0; i < 6; ++i) {
        const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[i]);
        KAI_CHECK(stmt.expr().kind() == ExprKind::Literal);
        const auto& literal = static_cast<const LiteralExpr&>(stmt.expr());
        KAI_CHECK(literal.literalKind() == expectedKinds[i]);
    }
}

// --- Top-level declaration classification ---

void testUnexpectedTopLevelToken() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "123");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    KAI_CHECK(result.error().kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(result.error().actual == TokenKind::IntegerLiteral);
}

void testBareUnsupportedTopLevelDeclarations() {
    const char* sources[] = {"struct Point {}", "enum E {}", "use foo"};

    for (const char* source : sources) {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", source);
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();

        KAI_CHECK(!result.has_value());
        if (result) {
            continue;
        }
        KAI_CHECK(result.error().kind == ParseErrorKind::UnsupportedSyntax);
    }
}

void testPublicUnsupportedDeclarationsAreUnsupportedSyntax() {
    const char* sources[] = {"pub struct Point {}", "pub enum E {}", "pub use foo"};

    for (const char* source : sources) {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", source);
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();

        KAI_CHECK(!result.has_value());
        if (result) {
            continue;
        }
        KAI_CHECK(result.error().kind == ParseErrorKind::UnsupportedSyntax);
    }
}

void testPubFollowedByNonDeclarationIsUnexpectedToken() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "pub 123");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    KAI_CHECK(result.error().kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(result.error().actual == TokenKind::IntegerLiteral);
}

// --- Expected-token preservation ---

void testMissingRightParenReportsExpectedToken() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() { print(\"a\" }");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::RightBrace);
    KAI_CHECK(error.expected == TokenKind::RightParen);
}

void testMissingClosingBraceReportsExpectedToken() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\")\n");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::EndOfFile);
    KAI_CHECK(error.expected == TokenKind::RightBrace);
}

// --- Lexical vs. syntactic vs. unsupported distinctions ---

void testInvalidLexerTokenWhereExpressionExpected() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    $\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
    KAI_CHECK(error.actual == TokenKind::Invalid);
}

void testSyntacticallyInvalidFunctionNameIsUnexpectedTokenNotInvalidToken() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn 123() {}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::IntegerLiteral);
}

void testUnsupportedLetInsideFunctionBody() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::KwLet);
}

void testUnsupportedBinaryOperator() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1 + 2\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::Plus);
}

// --- EOF inside function / block / call ---

void testEOFInsideFunctionParameterList() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main(");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.actual == TokenKind::EndOfFile);
    KAI_CHECK(error.expected == TokenKind::Identifier);
}

void testEOFInsideEmptyFunctionBody() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::EndOfFile);
    KAI_CHECK(error.expected == TokenKind::RightBrace);
}

void testEOFInsideCallArguments() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::EndOfFile);
}

// --- Malformed call-argument cases (no recovery, precise failure) ---

void testMalformedCallArgumentCases() {
    // print(
    {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\n}");
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();
        KAI_CHECK(!result.has_value());
    }
    // print(,
    {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(,\n}");
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();
        KAI_CHECK(!result.has_value());
    }
    // print("a",
    {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\",\n}");
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();
        KAI_CHECK(!result.has_value());
    }
    // print("a" "b")
    {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\" \"b\")\n}");
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();
        KAI_CHECK(!result.has_value());
        if (!result) {
            KAI_CHECK(result.error().actual == TokenKind::StringLiteral);
            KAI_CHECK(result.error().expected == TokenKind::RightParen);
        }
    }
    // print("a",) - no trailing comma allowed
    {
        SourceManager sm;
        const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    print(\"a\",)\n}");
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();
        KAI_CHECK(!result.has_value());
        if (!result) {
            KAI_CHECK(result.error().actual == TokenKind::RightParen);
        }
    }
}

// Not independently checkable via KAI_CHECK (no leak sanitizer wired
// into this harness), but exercises a case where several AST subtrees
// are successfully built and then discarded mid-parse when a later
// sibling fails. If RAII cleanup were broken, this would show up as a
// crash when this test binary runs, not as a silent KAI_CHECK failure.
void testPartiallyBuiltTreeCleansUpOnLaterFailure() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo(1, 2, 3, )\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();
    KAI_CHECK(!result.has_value());
}

} // namespace

int main() {
    testHelloWorldParsesFullShape();

    testPublicFunctionRecordsVisibility();
    testFunctionWithParamsAndReturnType();

    testEmptyBlock();
    testBlockWithBlankLineOnly();
    testSemicolonSeparatedStatements();
    testMixedNewlineAndSemicolonSeparators();

    testParenthesizedExpression();
    testZeroArgumentCall();
    testMultipleArguments();
    testChainedCalls();
    testAllSupportedLiteralForms();

    testUnexpectedTopLevelToken();
    testBareUnsupportedTopLevelDeclarations();
    testPublicUnsupportedDeclarationsAreUnsupportedSyntax();
    testPubFollowedByNonDeclarationIsUnexpectedToken();

    testMissingRightParenReportsExpectedToken();
    testMissingClosingBraceReportsExpectedToken();

    testInvalidLexerTokenWhereExpressionExpected();
    testSyntacticallyInvalidFunctionNameIsUnexpectedTokenNotInvalidToken();
    testUnsupportedLetInsideFunctionBody();
    testUnsupportedBinaryOperator();

    testEOFInsideFunctionParameterList();
    testEOFInsideEmptyFunctionBody();
    testEOFInsideCallArguments();

    testMalformedCallArgumentCases();
    testPartiallyBuiltTreeCleansUpOnLaterFailure();

    return kai::test::failureCount == 0 ? 0 : 1;
}
