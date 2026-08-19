#include "kai/parser/Parser.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>

using kai::FileId;
using kai::SourceManager;
using kai::TokenKind;
using kai::ast::ArrayLiteralExpr;
using kai::ast::ArrayTypeSyntax;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::BinaryOperator;
using kai::ast::BindingKind;
using kai::ast::CallExpr;
using kai::ast::DeclKind;
using kai::ast::ElseClause;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprStmt;
using kai::ast::ForStmt;
using kai::ast::FunctionDecl;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::IfBranch;
using kai::ast::IfStmt;
using kai::ast::IndexExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::MemberExpr;
using kai::ast::NamedTypeSyntax;
using kai::ast::ParenExpr;
using kai::ast::ReferenceMutability;
using kai::ast::ReferenceTypeSyntax;
using kai::ast::ReturnStmt;
using kai::ast::SliceTypeSyntax;
using kai::ast::SourceFile;
using kai::ast::StmtKind;
using kai::ast::TypeSyntaxKind;
using kai::ast::UnaryExpr;
using kai::ast::UnaryOperator;
using kai::ast::VarDeclStmt;
using kai::ast::WhileStmt;
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

void testArrayLiteralAtPrimaryPositionParses() {
    // `[` at primary-expression position is now ArrayLiteralExpr, not
    // UnsupportedSyntax (was true in Phase 1 of this milestone; this
    // Parser milestone implements it).
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, 2, 3]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::ArrayLiteral);
    const auto& array = static_cast<const ArrayLiteralExpr&>(exprStmt.expr());
    KAI_CHECK(array.elements().size() == 3);
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

// --- Variable declarations ---

void testLetInferredParsesVarDeclStmt() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::VarDecl);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(decl.binding() == BindingKind::Immutable);
    KAI_CHECK(sm.text(decl.name().span) == "x");
    KAI_CHECK(decl.type() == nullptr);
    KAI_CHECK(decl.initializer().kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(decl.initializer().span()) == "10");
    KAI_CHECK(sm.text(decl.span()) == "let x = 10");
}

void testLetTypedParsesVarDeclStmt() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: i32 = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(decl.binding() == BindingKind::Immutable);
    KAI_CHECK(decl.type() != nullptr);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Named);
    KAI_CHECK(sm.text(decl.type()->span()) == "i32");
}

void testMutInferredParsesVarDeclStmt() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    mut x = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(decl.binding() == BindingKind::Mutable);
    KAI_CHECK(decl.type() == nullptr);
}

void testMutTypedParsesVarDeclStmt() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    mut x: i32 = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(decl.binding() == BindingKind::Mutable);
    KAI_CHECK(decl.type() != nullptr);
    KAI_CHECK(sm.text(decl.type()->span()) == "i32");
}

void testVarDeclInitializerCallExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x = add(20, 22)\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.initializer().kind() == ExprKind::Call);
}

void testLetTypeMismatchStillParsesSyntactically() {
    // Semantic type checking does not exist yet - the parser records
    // syntax only (LANGUAGE_DESIGN.md / TYPE_SYSTEM.md §2).
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let age: i32 = \"twenty\"\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(sm.text(decl.type()->span()) == "i32");
    KAI_CHECK(decl.initializer().kind() == ExprKind::Literal);
    const auto& literal = static_cast<const LiteralExpr&>(decl.initializer());
    KAI_CHECK(literal.literalKind() == LiteralKind::String);
}

void testMalformedLetMissingName() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let = 10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::Equal);
    KAI_CHECK(error.expected == TokenKind::Identifier);
}

void testMalformedLetMissingEquals() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Equal);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

void testMalformedLetMissingInitializer() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x =\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

void testMalformedMutMissingName() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    mut\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
}

void testMalformedTypeAnnotation() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    mut x:\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

// --- Return statements ---

void testBareReturn() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::Return);
    const auto& stmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(stmt.value() == nullptr);
    KAI_CHECK(sm.text(stmt.span()) == "return");
}

void testReturnLiteral() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return 42\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& stmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(stmt.value() != nullptr);
    KAI_CHECK(stmt.value()->kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(stmt.span()) == "return 42");
}

void testReturnCall() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return add(a, b)\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& stmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.value() != nullptr);
    KAI_CHECK(stmt.value()->kind() == ExprKind::Call);
}

void testReturnBinaryExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return a + b\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& stmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.value() != nullptr);
    KAI_CHECK(stmt.value()->kind() == ExprKind::Binary);
    const auto& binary = static_cast<const BinaryExpr&>(*stmt.value());
    KAI_CHECK(binary.op() == BinaryOperator::Add);
}

void testNewlineAfterReturnMakesItBare() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return\n    print(\"next\")\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements().size() == 2);

    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::Return);
    const auto& returnStmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(returnStmt.value() == nullptr);

    KAI_CHECK(fn.body().statements()[1]->kind() == StmtKind::Expr);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Call);
}

void testReturnPlusFailsAtPlus() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    return +\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::Plus);
}

// --- Unary expressions ---

void testUnaryNegate() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    -x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Unary);
    const auto& unary = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(unary.op() == UnaryOperator::Negate);
    KAI_CHECK(sm.text(unary.operatorSpan()) == "-");
    KAI_CHECK(unary.operand().kind() == ExprKind::Identifier);
}

void testUnaryNot() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    !x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& unary = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(unary.op() == UnaryOperator::Not);
    KAI_CHECK(sm.text(unary.operatorSpan()) == "!");
}

void testUnaryRef() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    &x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& unary = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(unary.op() == UnaryOperator::Ref);
    KAI_CHECK(sm.text(unary.operatorSpan()) == "&");
}

void testUnaryRefMut() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    &mut x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& unary = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(unary.op() == UnaryOperator::RefMut);
    KAI_CHECK(sm.text(unary.operatorSpan()) == "&mut");
    KAI_CHECK(sm.text(unary.span()) == "&mut x");
}

void testUnaryRefMutWithSpaceBetweenAmpAndMut() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    & mut x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& unary = static_cast<const UnaryExpr&>(exprStmt.expr());

    // Whitespace between Amp and KwMut is not significant: operatorSpan
    // still runs from Amp.begin to KwMut.end, whitespace included.
    KAI_CHECK(unary.op() == UnaryOperator::RefMut);
    KAI_CHECK(sm.text(unary.operatorSpan()) == "& mut");
}

void testNestedUnaryNegateNegate() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    --x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(outer.op() == UnaryOperator::Negate);
    KAI_CHECK(outer.operand().kind() == ExprKind::Unary);
    const auto& inner = static_cast<const UnaryExpr&>(outer.operand());
    KAI_CHECK(inner.op() == UnaryOperator::Negate);
    KAI_CHECK(inner.operand().kind() == ExprKind::Identifier);
}

void testNestedUnaryNotNegate() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    !-x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const UnaryExpr&>(exprStmt.expr());

    KAI_CHECK(outer.op() == UnaryOperator::Not);
    const auto& inner = static_cast<const UnaryExpr&>(outer.operand());
    KAI_CHECK(inner.op() == UnaryOperator::Negate);
}

void testAmpAmpIsNotReinterpretedAsTwoRefOperators() {
    // `&&x` lexes as a single AmpAmp token (logical-and), never as two
    // Amp tokens - it must not be reinterpreted as `& &x`.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    &&x\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::AmpAmp);
}

// --- Binary expressions ---

void testEveryBinaryOperatorMapsCorrectly() {
    struct Case {
        const char* source;
        BinaryOperator op;
    };
    const Case cases[] = {
        {"a || b", BinaryOperator::Or},        {"a && b", BinaryOperator::And},
        {"a == b", BinaryOperator::Equal},      {"a != b", BinaryOperator::NotEqual},
        {"a < b", BinaryOperator::Less},        {"a <= b", BinaryOperator::LessEqual},
        {"a > b", BinaryOperator::Greater},     {"a >= b", BinaryOperator::GreaterEqual},
        {"a..b", BinaryOperator::Range},        {"a + b", BinaryOperator::Add},
        {"a - b", BinaryOperator::Subtract},    {"a * b", BinaryOperator::Multiply},
        {"a / b", BinaryOperator::Divide},      {"a % b", BinaryOperator::Modulo},
    };

    for (const Case& c : cases) {
        SourceManager sm;
        const std::string source = std::string("fn main() {\n    ") + c.source + "\n}";
        const FileId file = sm.addVirtualFile("a.kai", source);
        Parser parser(sm, file);
        auto result = parser.parseSourceFile();

        KAI_CHECK(result.has_value());
        if (!result) {
            continue;
        }
        const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
        const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
        KAI_CHECK(exprStmt.expr().kind() == ExprKind::Binary);
        const auto& binary = static_cast<const BinaryExpr&>(exprStmt.expr());
        KAI_CHECK(binary.op() == c.op);
        KAI_CHECK(binary.left().kind() == ExprKind::Identifier);
        KAI_CHECK(binary.right().kind() == ExprKind::Identifier);
    }
}

void testMultiplicationBindsTighterThanAddition() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1 + 2 * 3\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& add = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(add.op() == BinaryOperator::Add);
    KAI_CHECK(add.left().kind() == ExprKind::Literal);
    KAI_CHECK(add.right().kind() == ExprKind::Binary);
    const auto& mul = static_cast<const BinaryExpr&>(add.right());
    KAI_CHECK(mul.op() == BinaryOperator::Multiply);
}

void testParenthesesOverridePrecedence() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    (1 + 2) * 3\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& mul = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(mul.op() == BinaryOperator::Multiply);
    KAI_CHECK(mul.left().kind() == ExprKind::Paren);
    const auto& paren = static_cast<const ParenExpr&>(mul.left());
    KAI_CHECK(paren.inner().kind() == ExprKind::Binary);
    const auto& add = static_cast<const BinaryExpr&>(paren.inner());
    KAI_CHECK(add.op() == BinaryOperator::Add);
    KAI_CHECK(mul.right().kind() == ExprKind::Literal);
}

void testLogicalAndBindsTighterThanLogicalOr() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a || b && c\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& orExpr = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(orExpr.op() == BinaryOperator::Or);
    KAI_CHECK(orExpr.left().kind() == ExprKind::Identifier);
    KAI_CHECK(orExpr.right().kind() == ExprKind::Binary);
    const auto& andExpr = static_cast<const BinaryExpr&>(orExpr.right());
    KAI_CHECK(andExpr.op() == BinaryOperator::And);
}

void testEqualityCombinationWithLogicalOr() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a == b || c == d\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& orExpr = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(orExpr.op() == BinaryOperator::Or);
    KAI_CHECK(orExpr.left().kind() == ExprKind::Binary);
    KAI_CHECK(orExpr.right().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(orExpr.left()).op() == BinaryOperator::Equal);
    KAI_CHECK(static_cast<const BinaryExpr&>(orExpr.right()).op() == BinaryOperator::Equal);
}

void testRangeBasic() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    0..10\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& range = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(range.op() == BinaryOperator::Range);
    KAI_CHECK(sm.text(range.left().span()) == "0");
    KAI_CHECK(sm.text(range.right().span()) == "10");
}

void testRangeOperandsAreFullAdditiveExpressions() {
    // GRAMMAR.md §32/§26: range's operand production is additive, so
    // `0..10 + 1` must parse as `0..(10 + 1)`, not `(0..10) + 1`.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    0..10 + 1\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& range = static_cast<const BinaryExpr&>(exprStmt.expr());

    KAI_CHECK(range.op() == BinaryOperator::Range);
    KAI_CHECK(range.left().kind() == ExprKind::Literal);
    KAI_CHECK(range.right().kind() == ExprKind::Binary);
    const auto& add = static_cast<const BinaryExpr&>(range.right());
    KAI_CHECK(add.op() == BinaryOperator::Add);
}

void testRangeChainFails() {
    // parseRange consumes at most one ".." (GRAMMAR.md §32 uses "[...]",
    // not "{...}"); a second ".." must not form a chain.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1..2..3\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::DotDot);
}

// --- Assignment expressions ---

void testSimpleAssignment() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a = b\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Assignment);
    const auto& assign = static_cast<const AssignmentExpr&>(exprStmt.expr());

    KAI_CHECK(assign.target().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(assign.target().span()) == "a");
    KAI_CHECK(assign.value().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(assign.value().span()) == "b");
    KAI_CHECK(sm.text(assign.operatorSpan()) == "=");
}

void testChainedAssignmentIsRightAssociative() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a = b = c\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const AssignmentExpr&>(exprStmt.expr());

    KAI_CHECK(sm.text(outer.target().span()) == "a");
    KAI_CHECK(outer.value().kind() == ExprKind::Assignment);
    const auto& inner = static_cast<const AssignmentExpr&>(outer.value());
    KAI_CHECK(sm.text(inner.target().span()) == "b");
    KAI_CHECK(sm.text(inner.value().span()) == "c");
}

void testAssignmentAroundBinaryExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a = b + c * d\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assign = static_cast<const AssignmentExpr&>(exprStmt.expr());

    KAI_CHECK(sm.text(assign.target().span()) == "a");
    KAI_CHECK(assign.value().kind() == ExprKind::Binary);
    const auto& add = static_cast<const BinaryExpr&>(assign.value());
    KAI_CHECK(add.op() == BinaryOperator::Add);
    KAI_CHECK(add.right().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(add.right()).op() == BinaryOperator::Multiply);
}

void testAssignmentTargetNotRestrictedToIdentifier() {
    // No lvalue check in the parser or AST - semantic analysis's job.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1 = 2\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assign = static_cast<const AssignmentExpr&>(exprStmt.expr());
    KAI_CHECK(assign.target().kind() == ExprKind::Literal);
}

void testAssignmentTargetCanBeBinaryExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    a + b = c\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assign = static_cast<const AssignmentExpr&>(exprStmt.expr());

    KAI_CHECK(assign.target().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(assign.target()).op() == BinaryOperator::Add);
    KAI_CHECK(assign.value().kind() == ExprKind::Identifier);
}

// --- Regression / error behavior ---

void testInvalidTokenInNewExpressionPath() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1 + $\n}");
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

void testInvalidTokenInsideVarInitializer() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x = $\n}");
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

void testMemberAccessAfterCallParses() {
    // `.` after a postfix expression is now MemberExpr, not
    // UnsupportedSyntax.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo().bar\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Member);
    const auto& member = static_cast<const MemberExpr&>(exprStmt.expr());
    KAI_CHECK(sm.text(member.member().span) == "bar");
    KAI_CHECK(member.object().kind() == ExprKind::Call);
}

void testIndexingAtPostfixPositionParses() {
    // `[` after a postfix expression is now IndexExpr, not
    // UnsupportedSyntax.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Index);
    const auto& index = static_cast<const IndexExpr&>(exprStmt.expr());
    KAI_CHECK(sm.text(index.object().span()) == "values");
    KAI_CHECK(sm.text(index.index().span()) == "0");
}

void testTryOperatorRemainsUnsupportedSyntax() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    result?\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::Question);
}

void testEOFAfterBinaryOperator() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    1 +");
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

void testEOFAfterUnaryOperator() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    -");
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

// --- If statements ---

void testIfTrueEmptyBodyParses() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if true {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::If);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(ifStmt.branches().size() == 1);
    KAI_CHECK(!ifStmt.elseClause().has_value());
    KAI_CHECK(ifStmt.branches()[0].condition->kind() == ExprKind::Literal);
    KAI_CHECK(ifStmt.branches()[0].body->statements().empty());
}

void testIfBodyContainsExpressionStatement() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if true {\n        print(x)\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(ifStmt.branches()[0].body->statements().size() == 1);
    KAI_CHECK(ifStmt.branches()[0].body->statements()[0]->kind() == StmtKind::Expr);
}

void testIfElseBothBranchesEmpty() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a > b {} else {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(ifStmt.branches().size() == 1);
    KAI_CHECK(ifStmt.branches()[0].condition->kind() == ExprKind::Binary);
    const auto& cond = static_cast<const BinaryExpr&>(*ifStmt.branches()[0].condition);
    KAI_CHECK(cond.op() == BinaryOperator::Greater);
    KAI_CHECK(ifStmt.elseClause().has_value());
    KAI_CHECK(ifStmt.elseClause()->body->statements().empty());
}

void testIfElseIfElseIfElseBranchCountOrderAndConditions() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {} else if b {} else if c {} else {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(ifStmt.branches().size() == 3);
    KAI_CHECK(sm.text(ifStmt.branches()[0].condition->span()) == "a");
    KAI_CHECK(sm.text(ifStmt.branches()[1].condition->span()) == "b");
    KAI_CHECK(sm.text(ifStmt.branches()[2].condition->span()) == "c");
    KAI_CHECK(ifStmt.elseClause().has_value());
}

void testIfBranchSpansCoverIfAndElseIfKeywords() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {} else if b {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    // Initial branch span begins at `if`, not the condition.
    KAI_CHECK(sm.text(ifStmt.branches()[0].span) == "if a {}");
    // else-if branch span begins at `else`, not the following `if`.
    KAI_CHECK(sm.text(ifStmt.branches()[1].span) == "else if b {}");
}

void testElseClauseSpanBeginsAtElseKeyword() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {} else {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(ifStmt.elseClause().has_value());
    KAI_CHECK(sm.text(ifStmt.elseClause()->span) == "else {}");
}

void testFullIfStmtSpanCoversIfThroughFinalElse() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {} else if b {} else {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(sm.text(ifStmt.span()) == "if a {} else if b {} else {}");
}

void testFullIfStmtSpanWithNoElseEndsAtLastBranch() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {} else if b {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(!ifStmt.elseClause().has_value());
    KAI_CHECK(sm.text(ifStmt.span()) == "if a {} else if b {}");
}

void testIfNonBoolConditionSucceedsSyntactically() {
    // No bool/type validation belongs in the parser.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if 42 {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(ifStmt.branches()[0].condition->kind() == ExprKind::Literal);
}

// --- Newline-before-else behavior ---

void testNewlineBeforeElseSucceeds() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n    }\n    else {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(ifStmt.elseClause().has_value());
}

void testMultipleNewlinesBeforeElseSucceeds() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n    }\n\n\n    else {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(ifStmt.elseClause().has_value());
}

void testNewlineBeforeElseIfSucceeds() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n    }\n    else if b {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(ifStmt.branches().size() == 2);
    KAI_CHECK(!ifStmt.elseClause().has_value());
}

void testSemicolonBeforeElseFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {}; else {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::KwElse);
}

void testElseNewlineIfIsInvalid() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n    }\n    else\n    if b {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::LeftBrace);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

void testElseNewlineBraceIsInvalid() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n    }\n    else\n    {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::LeftBrace);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

void testNewlineAfterIfKeywordIsInvalid() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if\n    true {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

// --- Nested if / dangling else ---

void testNestedIfOuterElseBindsToOuter() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if a {\n        if b {\n        }\n    } else {\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& outer = static_cast<const IfStmt&>(*fn.body().statements()[0]);

    // The outer if has the else - it did not get attached to the inner one.
    KAI_CHECK(outer.branches().size() == 1);
    KAI_CHECK(outer.elseClause().has_value());

    KAI_CHECK(outer.branches()[0].body->statements().size() == 1);
    KAI_CHECK(outer.branches()[0].body->statements()[0]->kind() == StmtKind::If);
    const auto& inner = static_cast<const IfStmt&>(*outer.branches()[0].body->statements()[0]);
    KAI_CHECK(inner.branches().size() == 1);
    KAI_CHECK(!inner.elseClause().has_value());
}

// --- While statements ---

void testWhileTrueEmptyBodyParses() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while true {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::While);
    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(whileStmt.condition().kind() == ExprKind::Literal);
    KAI_CHECK(whileStmt.body().statements().empty());
}

void testWhileWithAssignmentBody() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while i < 10 {\n        i = i + 1\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(whileStmt.condition().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(whileStmt.condition()).op() == BinaryOperator::Less);
    KAI_CHECK(whileStmt.body().statements().size() == 1);
    const auto& bodyStmt = static_cast<const ExprStmt&>(*whileStmt.body().statements()[0]);
    KAI_CHECK(bodyStmt.expr().kind() == ExprKind::Assignment);
}

void testWhileNonBoolConditionSucceedsSyntactically() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while 42 {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(whileStmt.condition().kind() == ExprKind::Literal);
}

void testWhileFullSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while a {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(whileStmt.span()) == "while a {}");
}

void testMalformedWhileMissingCondition() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::LeftBrace);
}

// --- For statements ---

void testForRangeIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i in 0..10 {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.body().statements()[0]->kind() == StmtKind::For);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(sm.text(forStmt.variable().span) == "i");
    KAI_CHECK(forStmt.iterable().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(forStmt.iterable()).op() == BinaryOperator::Range);
    KAI_CHECK(forStmt.body().statements().empty());
}

void testForIdentifierIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for value in values {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(sm.text(forStmt.variable().span) == "value");
    KAI_CHECK(forStmt.iterable().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(forStmt.iterable().span()) == "values");
}

void testForBodyContainsCall() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i in 0..10 {\n        print(i)\n    }\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(forStmt.body().statements().size() == 1);
    const auto& bodyStmt = static_cast<const ExprStmt&>(*forStmt.body().statements()[0]);
    KAI_CHECK(bodyStmt.expr().kind() == ExprKind::Call);
}

void testForVariableIdentifierSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for index in 0..10 {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(forStmt.variable().span) == "index");
}

void testForFullSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i in 0..10 {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(forStmt.span()) == "for i in 0..10 {}");
}

void testMalformedForMissingIdentifier() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for in values {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
    KAI_CHECK(error.actual == TokenKind::KwIn);
}

void testMalformedForMissingIn() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i values {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::KwIn);
    KAI_CHECK(error.actual == TokenKind::Identifier);
}

void testMalformedForMissingIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i in {}\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::LeftBrace);
}

void testMalformedForMissingBody() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for i in values\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::LeftBrace);
}

// --- InvalidToken in control-flow headers ---

void testInvalidTokenInIfCondition() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    if $ {}\n}");
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

void testInvalidTokenInWhileCondition() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    while $ {}\n}");
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

void testInvalidTokenInForIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    for x in $ {}\n}");
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

// --- Array literals ---

void testEmptyArrayLiteral() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    []\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::ArrayLiteral);
    KAI_CHECK(static_cast<const ArrayLiteralExpr&>(exprStmt.expr()).elements().empty());
}

void testSingleElementArrayLiteral() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(static_cast<const ArrayLiteralExpr&>(exprStmt.expr()).elements().size() == 1);
}

void testTwoElementArrayLiteral() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, 2]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(static_cast<const ArrayLiteralExpr&>(exprStmt.expr()).elements().size() == 2);
}

void testNestedArrayLiterals() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [[1, 2], [3, 4]]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const ArrayLiteralExpr&>(exprStmt.expr());

    KAI_CHECK(outer.elements().size() == 2);
    KAI_CHECK(outer.elements()[0]->kind() == ExprKind::ArrayLiteral);
    const auto& first = static_cast<const ArrayLiteralExpr&>(*outer.elements()[0]);
    KAI_CHECK(first.elements().size() == 2);
    KAI_CHECK(sm.text(first.elements()[0]->span()) == "1");
    KAI_CHECK(sm.text(first.elements()[1]->span()) == "2");
    const auto& second = static_cast<const ArrayLiteralExpr&>(*outer.elements()[1]);
    KAI_CHECK(sm.text(second.elements()[0]->span()) == "3");
    KAI_CHECK(sm.text(second.elements()[1]->span()) == "4");
}

void testMultilineArrayLiteral() {
    // Lexer suppresses Newline while bracketDepth_ > 0 - no parser-side
    // newline handling is needed for this to work.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x = [\n        1,\n        2\n    ]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.initializer().kind() == ExprKind::ArrayLiteral);
    KAI_CHECK(static_cast<const ArrayLiteralExpr&>(decl.initializer()).elements().size() == 2);
}

void testMixedTypeArrayLiteralSucceedsSyntactically() {
    // No element-type inference or uniformity checking in the parser.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, \"two\", true]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& array = static_cast<const ArrayLiteralExpr&>(exprStmt.expr());

    KAI_CHECK(array.elements().size() == 3);
    KAI_CHECK(static_cast<const LiteralExpr&>(*array.elements()[0]).literalKind() == LiteralKind::Integer);
    KAI_CHECK(static_cast<const LiteralExpr&>(*array.elements()[1]).literalKind() == LiteralKind::String);
    KAI_CHECK(static_cast<const LiteralExpr&>(*array.elements()[2]).literalKind() == LiteralKind::Bool);
}

void testArrayLiteralExactSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, 2, 3]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(exprStmt.expr().span()) == "[1, 2, 3]");
}

void testArrayLiteralTrailingCommaFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, 2,]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::RightBracket);
}

void testArrayLiteralMissingCommaFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1 2]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
    KAI_CHECK(error.actual == TokenKind::IntegerLiteral);
}

void testArrayLiteralMissingClosingBracketFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [1, 2\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
}

void testArrayLiteralInvalidTokenElementFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    [$]\n}");
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

// --- Indexing ---

void testIndexArbitraryExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[i + 1]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& index = static_cast<const IndexExpr&>(exprStmt.expr());
    KAI_CHECK(index.index().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(index.index()).op() == BinaryOperator::Add);
}

void testIndexNestedMatrixShape() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    matrix[i][j]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const IndexExpr&>(exprStmt.expr());

    KAI_CHECK(sm.text(outer.index().span()) == "j");
    KAI_CHECK(outer.object().kind() == ExprKind::Index);
    const auto& inner = static_cast<const IndexExpr&>(outer.object());
    KAI_CHECK(sm.text(inner.object().span()) == "matrix");
    KAI_CHECK(sm.text(inner.index().span()) == "i");
    KAI_CHECK(sm.text(outer.span()) == "matrix[i][j]");
}

void testIndexAfterCall() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    call()[0]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& index = static_cast<const IndexExpr&>(exprStmt.expr());
    KAI_CHECK(index.object().kind() == ExprKind::Call);
}

void testIndexThenMember() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0].member\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Member);
    const auto& member = static_cast<const MemberExpr&>(exprStmt.expr());
    KAI_CHECK(sm.text(member.member().span) == "member");
    KAI_CHECK(member.object().kind() == ExprKind::Index);
}

void testIndexEmptyBracketsFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::RightBracket);
}

void testIndexUnterminatedFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[");
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

void testIndexMissingClosingAfterExprFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
    KAI_CHECK(error.actual == TokenKind::EndOfFile);
}

void testIndexExtraTokenFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0 1]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
    KAI_CHECK(error.actual == TokenKind::IntegerLiteral);
}

void testIndexCommaFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0, 1]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
    KAI_CHECK(error.actual == TokenKind::Comma);
}

void testIndexInvalidTokenFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[$]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
}

void testIndexThenQuestionRemainsUnsupportedSyntax() {
    // Adding indexing/member postfix forms must not regress `?`.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values[0]?\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::Question);
}

// --- Member access ---

void testMemberThenCall() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo.bar()\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Call);
    const auto& call = static_cast<const CallExpr&>(exprStmt.expr());
    KAI_CHECK(call.callee().kind() == ExprKind::Member);
    const auto& member = static_cast<const MemberExpr&>(call.callee());
    KAI_CHECK(sm.text(member.member().span) == "bar");
    KAI_CHECK(sm.text(member.object().span()) == "foo");
}

void testMemberOnIntegerLiteral() {
    // Syntactically valid: the parser does not check whether `42` has
    // members. Semantic analysis's job.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    42.foo\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Member);
    const auto& member = static_cast<const MemberExpr&>(exprStmt.expr());
    KAI_CHECK(member.object().kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(member.member().span) == "foo");
}

void testChainedMemberAccess() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    foo.bar.baz\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const MemberExpr&>(exprStmt.expr());

    KAI_CHECK(sm.text(outer.member().span) == "baz");
    KAI_CHECK(outer.object().kind() == ExprKind::Member);
    const auto& inner = static_cast<const MemberExpr&>(outer.object());
    KAI_CHECK(sm.text(inner.member().span) == "bar");
    KAI_CHECK(sm.text(inner.object().span()) == "foo");
}

void testMemberExactIdentifierSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    values.len\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& member = static_cast<const MemberExpr&>(exprStmt.expr());
    KAI_CHECK(sm.text(member.member().span) == "len");
    KAI_CHECK(sm.text(member.span()) == "values.len");
}

void testMemberMissingIdentifierFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    value.\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
    KAI_CHECK(error.actual == TokenKind::Newline);
}

void testMemberNumericFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    value.123\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
    KAI_CHECK(error.actual == TokenKind::IntegerLiteral);
}

void testMemberInvalidTokenFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    value.$\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
}

void testDotDotRemainsRangeNotMemberAccess() {
    // `..` lexes as a single DotDot token, never two Dot tokens - this
    // must remain a Range BinaryExpr, not a malformed member access.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    value..other\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(exprStmt.expr()).op() == BinaryOperator::Range);
}

// --- Slice types ---

void testSliceTypeBasic() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Slice);
    const auto& slice = static_cast<const SliceTypeSyntax&>(*decl.type());
    KAI_CHECK(sm.text(slice.element().span()) == "i32");
}

void testSliceTypeFloat() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [f64] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(static_cast<const SliceTypeSyntax&>(*decl.type()).element().span()) == "f64");
}

void testSliceTypeUserType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [MyType] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(static_cast<const SliceTypeSyntax&>(*decl.type()).element().span()) == "MyType");
}

void testSliceTypeOfReference() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [&str] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& slice = static_cast<const SliceTypeSyntax&>(*decl.type());
    KAI_CHECK(slice.element().kind() == TypeSyntaxKind::Reference);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(slice.element());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(reference.referent().span()) == "str");
}

void testSliceOfFixedArray() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [[i32; 4]] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Slice);
    const auto& outer = static_cast<const SliceTypeSyntax&>(*decl.type());
    KAI_CHECK(outer.element().kind() == TypeSyntaxKind::Array);
    const auto& inner = static_cast<const ArrayTypeSyntax&>(outer.element());
    KAI_CHECK(sm.text(inner.element().span()) == "i32");
    KAI_CHECK(sm.text(inner.length().span()) == "4");
}

// --- Fixed array types ---

void testFixedArrayType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; 4] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Array);
    const auto& array = static_cast<const ArrayTypeSyntax&>(*decl.type());
    KAI_CHECK(sm.text(array.element().span()) == "i32");
}

void testFixedArrayLengthIsIntegerLiteralExpr() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; 4] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& array = static_cast<const ArrayTypeSyntax&>(*decl.type());
    KAI_CHECK(array.length().kind() == ExprKind::Literal);
    const auto& literal = static_cast<const LiteralExpr&>(array.length());
    KAI_CHECK(literal.literalKind() == LiteralKind::Integer);
    KAI_CHECK(sm.text(literal.span()) == "4");
}

void testFixedArrayExactSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; 4] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(sm.text(decl.type()->span()) == "[i32; 4]");
}

void testFixedArrayNonLiteralLengthFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; N] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::IntegerLiteral);
    KAI_CHECK(error.actual == TokenKind::Identifier);
}

void testFixedArrayExpressionLengthFailsAtPlus() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; 2 + 2] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::RightBracket);
    KAI_CHECK(error.actual == TokenKind::Plus);
}

void testFixedArrayNegativeLengthFails() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; -1] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::IntegerLiteral);
    KAI_CHECK(error.actual == TokenKind::Minus);
}

void testEmptyBracketTypeFails() {
    // `[]` is never a valid type - the grammar requires an element type
    // after `[` in type position, unlike expression position where `[]`
    // is a valid empty ArrayLiteralExpr.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::RightBracket);
}

// --- Reference types ---

void testReferenceImmutable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Reference);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(reference.operatorSpan()) == "&");
    KAI_CHECK(sm.text(reference.referent().span()) == "i32");
}

void testReferenceMutable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &mut i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(sm.text(reference.operatorSpan()) == "&mut");
}

void testReferenceMutableWithSpace() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: & mut i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(sm.text(reference.operatorSpan()) == "& mut");
}

void testReferenceToSlice() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &[i32] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Slice);
    KAI_CHECK(sm.text(static_cast<const SliceTypeSyntax&>(reference.referent()).element().span()) == "i32");
}

void testReferenceMutableToSlice() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &mut [i32] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Slice);
}

void testReferenceToFixedArray() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &[i32; 4] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& reference = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Array);
    const auto& array = static_cast<const ArrayTypeSyntax&>(reference.referent());
    KAI_CHECK(sm.text(array.length().span()) == "4");
}

void testNestedReferenceMutableToImmutable() {
    // `&mut &i32`: Amp, KwMut, Amp, Identifier - two distinct, non-
    // adjacent Amp tokens (separated by "mut "), so this composes via
    // plain recursion.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &mut &i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& outer = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(outer.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(outer.referent().kind() == TypeSyntaxKind::Reference);
    const auto& inner = static_cast<const ReferenceTypeSyntax&>(outer.referent());
    KAI_CHECK(inner.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(inner.referent().span()) == "i32");
}

void testDoubleAmpDoesNotFormNestedReference() {
    // `&&i32` lexes as a single AmpAmp token (logical-and), never as two
    // Amp tokens - it is not accepted as a doubled reference type.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &&i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.expected == TokenKind::Identifier);
    KAI_CHECK(error.actual == TokenKind::AmpAmp);
}

void testSpacedDoubleAmpFormsNestedReference() {
    // `& &i32`: unlike `&&i32`, the two `&` are separated by whitespace
    // so the Lexer never merges them into AmpAmp - they lex as two
    // distinct Amp tokens, and parseReferenceTypeSyntax's recursion into
    // parseTypeSyntax() for the referent naturally composes them into
    // nested ReferenceTypeSyntax nodes.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: & &i32 = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Reference);
    const auto& outer = static_cast<const ReferenceTypeSyntax&>(*decl.type());
    KAI_CHECK(outer.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(outer.operatorSpan()) == "&");

    KAI_CHECK(outer.referent().kind() == TypeSyntaxKind::Reference);
    const auto& inner = static_cast<const ReferenceTypeSyntax&>(outer.referent());
    KAI_CHECK(inner.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(inner.operatorSpan()) == "&");

    KAI_CHECK(inner.referent().kind() == TypeSyntaxKind::Named);
    KAI_CHECK(sm.text(inner.referent().span()) == "i32");
}

// --- Type-position integration ---

void testSliceTypeInFunctionParameter() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn sum(values: [i32]) -> i32 {\n    return 0\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.params()[0].type->kind() == TypeSyntaxKind::Slice);
}

void testReferenceTypeInFunctionReturnType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn get() -> &i32 {\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    KAI_CHECK(fn.returnType() != nullptr);
    KAI_CHECK(fn.returnType()->kind() == TypeSyntaxKind::Reference);
}

void testArrayTypeInVariableAnnotation() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; 4] = [1, 2, 3, 4]\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(result.has_value());
    if (!result) {
        return;
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(decl.type()->kind() == TypeSyntaxKind::Array);
    KAI_CHECK(decl.initializer().kind() == ExprKind::ArrayLiteral);
}

// --- Unsupported regressions ---

void testGenericTypeRemainsUnsupportedSyntax() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: Result<i32, E> = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::Less);
}

void testUnitTypeRemainsUnsupportedSyntax() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: () = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::LeftParen);
}

// --- InvalidToken ---

void testInvalidTokenInArrayLength() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: [i32; $] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
}

void testInvalidTokenInReferenceReferent() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &$ = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
}

void testInvalidTokenInBracketTypeElement() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    let x: &[$] = y\n}");
    Parser parser(sm, file);
    auto result = parser.parseSourceFile();

    KAI_CHECK(!result.has_value());
    if (result) {
        return;
    }
    const ParseError& error = result.error();
    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
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
    testArrayLiteralAtPrimaryPositionParses();

    testEOFInsideFunctionParameterList();
    testEOFInsideEmptyFunctionBody();
    testEOFInsideCallArguments();

    testMalformedCallArgumentCases();
    testPartiallyBuiltTreeCleansUpOnLaterFailure();

    testLetInferredParsesVarDeclStmt();
    testLetTypedParsesVarDeclStmt();
    testMutInferredParsesVarDeclStmt();
    testMutTypedParsesVarDeclStmt();
    testVarDeclInitializerCallExpression();
    testLetTypeMismatchStillParsesSyntactically();
    testMalformedLetMissingName();
    testMalformedLetMissingEquals();
    testMalformedLetMissingInitializer();
    testMalformedMutMissingName();
    testMalformedTypeAnnotation();

    testBareReturn();
    testReturnLiteral();
    testReturnCall();
    testReturnBinaryExpression();
    testNewlineAfterReturnMakesItBare();
    testReturnPlusFailsAtPlus();

    testUnaryNegate();
    testUnaryNot();
    testUnaryRef();
    testUnaryRefMut();
    testUnaryRefMutWithSpaceBetweenAmpAndMut();
    testNestedUnaryNegateNegate();
    testNestedUnaryNotNegate();
    testAmpAmpIsNotReinterpretedAsTwoRefOperators();

    testEveryBinaryOperatorMapsCorrectly();
    testMultiplicationBindsTighterThanAddition();
    testParenthesesOverridePrecedence();
    testLogicalAndBindsTighterThanLogicalOr();
    testEqualityCombinationWithLogicalOr();
    testRangeBasic();
    testRangeOperandsAreFullAdditiveExpressions();
    testRangeChainFails();

    testSimpleAssignment();
    testChainedAssignmentIsRightAssociative();
    testAssignmentAroundBinaryExpression();
    testAssignmentTargetNotRestrictedToIdentifier();
    testAssignmentTargetCanBeBinaryExpression();

    testInvalidTokenInNewExpressionPath();
    testInvalidTokenInsideVarInitializer();
    testMemberAccessAfterCallParses();
    testIndexingAtPostfixPositionParses();
    testTryOperatorRemainsUnsupportedSyntax();
    testEOFAfterBinaryOperator();
    testEOFAfterUnaryOperator();

    testIfTrueEmptyBodyParses();
    testIfBodyContainsExpressionStatement();
    testIfElseBothBranchesEmpty();
    testIfElseIfElseIfElseBranchCountOrderAndConditions();
    testIfBranchSpansCoverIfAndElseIfKeywords();
    testElseClauseSpanBeginsAtElseKeyword();
    testFullIfStmtSpanCoversIfThroughFinalElse();
    testFullIfStmtSpanWithNoElseEndsAtLastBranch();
    testIfNonBoolConditionSucceedsSyntactically();

    testNewlineBeforeElseSucceeds();
    testMultipleNewlinesBeforeElseSucceeds();
    testNewlineBeforeElseIfSucceeds();
    testSemicolonBeforeElseFails();
    testElseNewlineIfIsInvalid();
    testElseNewlineBraceIsInvalid();
    testNewlineAfterIfKeywordIsInvalid();

    testNestedIfOuterElseBindsToOuter();

    testWhileTrueEmptyBodyParses();
    testWhileWithAssignmentBody();
    testWhileNonBoolConditionSucceedsSyntactically();
    testWhileFullSpan();
    testMalformedWhileMissingCondition();

    testForRangeIterable();
    testForIdentifierIterable();
    testForBodyContainsCall();
    testForVariableIdentifierSpan();
    testForFullSpan();
    testMalformedForMissingIdentifier();
    testMalformedForMissingIn();
    testMalformedForMissingIterable();
    testMalformedForMissingBody();

    testInvalidTokenInIfCondition();
    testInvalidTokenInWhileCondition();
    testInvalidTokenInForIterable();

    testEmptyArrayLiteral();
    testSingleElementArrayLiteral();
    testTwoElementArrayLiteral();
    testNestedArrayLiterals();
    testMultilineArrayLiteral();
    testMixedTypeArrayLiteralSucceedsSyntactically();
    testArrayLiteralExactSpan();
    testArrayLiteralTrailingCommaFails();
    testArrayLiteralMissingCommaFails();
    testArrayLiteralMissingClosingBracketFails();
    testArrayLiteralInvalidTokenElementFails();

    testIndexArbitraryExpression();
    testIndexNestedMatrixShape();
    testIndexAfterCall();
    testIndexThenMember();
    testIndexEmptyBracketsFails();
    testIndexUnterminatedFails();
    testIndexMissingClosingAfterExprFails();
    testIndexExtraTokenFails();
    testIndexCommaFails();
    testIndexInvalidTokenFails();
    testIndexThenQuestionRemainsUnsupportedSyntax();

    testMemberThenCall();
    testMemberOnIntegerLiteral();
    testChainedMemberAccess();
    testMemberExactIdentifierSpan();
    testMemberMissingIdentifierFails();
    testMemberNumericFails();
    testMemberInvalidTokenFails();
    testDotDotRemainsRangeNotMemberAccess();

    testSliceTypeBasic();
    testSliceTypeFloat();
    testSliceTypeUserType();
    testSliceTypeOfReference();
    testSliceOfFixedArray();

    testFixedArrayType();
    testFixedArrayLengthIsIntegerLiteralExpr();
    testFixedArrayExactSpan();
    testFixedArrayNonLiteralLengthFails();
    testFixedArrayExpressionLengthFailsAtPlus();
    testFixedArrayNegativeLengthFails();
    testEmptyBracketTypeFails();

    testReferenceImmutable();
    testReferenceMutable();
    testReferenceMutableWithSpace();
    testReferenceToSlice();
    testReferenceMutableToSlice();
    testReferenceToFixedArray();
    testNestedReferenceMutableToImmutable();
    testDoubleAmpDoesNotFormNestedReference();
    testSpacedDoubleAmpFormsNestedReference();

    testSliceTypeInFunctionParameter();
    testReferenceTypeInFunctionReturnType();
    testArrayTypeInVariableAnnotation();

    testGenericTypeRemainsUnsupportedSyntax();
    testUnitTypeRemainsUnsupportedSyntax();

    testInvalidTokenInArrayLength();
    testInvalidTokenInReferenceReferent();
    testInvalidTokenInBracketTypeElement();

    return kai::test::failureCount == 0 ? 0 : 1;
}
