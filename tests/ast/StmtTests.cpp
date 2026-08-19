#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::BindingKind;
using kai::ast::BlockStmt;
using kai::ast::CallExpr;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::ExprStmt;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::NamedTypeSyntax;
using kai::ast::ReturnStmt;
using kai::ast::StmtKind;
using kai::ast::StmtPtr;
using kai::ast::TypeSyntaxPtr;
using kai::ast::VarDeclStmt;

namespace {

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

void testExprStmtOwnsExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "42");
    const SourceSpan span = spanOf(file, 0, 2);

    ExprPtr literal = std::make_unique<LiteralExpr>(LiteralKind::Integer, span);
    ExprStmt stmt(std::move(literal), span);

    KAI_CHECK(stmt.kind() == StmtKind::Expr);
    KAI_CHECK(stmt.span() == span);
    KAI_CHECK(stmt.expr().span() == span);
    KAI_CHECK(literal == nullptr);
}

void testBlockStmtOwnsAndMovesStatementList() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "{ print(\"Hello from KAI\") }");

    const SourceSpan calleeSpan = spanOf(file, 2, 7);   // print
    const SourceSpan argSpan = spanOf(file, 8, 24);     // "Hello from KAI"
    const SourceSpan callSpan = spanOf(file, 2, 25);    // print(...)
    const SourceSpan blockSpan = spanOf(file, 0, 27);   // { ... }

    ExprPtr callee = std::make_unique<IdentifierExpr>(Identifier{calleeSpan}, calleeSpan);
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<LiteralExpr>(LiteralKind::String, argSpan));
    ExprPtr call = std::make_unique<CallExpr>(std::move(callee), std::move(args), callSpan);

    std::vector<StmtPtr> statements;
    statements.push_back(std::make_unique<ExprStmt>(std::move(call), callSpan));

    BlockStmt block(std::move(statements), blockSpan);

    KAI_CHECK(block.kind() == StmtKind::Block);
    KAI_CHECK(block.span() == blockSpan);
    KAI_CHECK(block.statements().size() == 1);
    KAI_CHECK(block.statements()[0]->kind() == StmtKind::Expr);

    // Ownership actually transferred out of the local vector.
    KAI_CHECK(statements.empty());
}

void testBlockStmtSupportsEmptyBody() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "{}");
    const SourceSpan span = spanOf(file, 0, 2);

    BlockStmt block(std::vector<StmtPtr>{}, span);

    KAI_CHECK(block.statements().empty());
}

// --- VarDeclStmt ---

void testVarDeclStmtImmutableWithoutType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);  // x
    const SourceSpan initSpan = spanOf(file, 8, 10); // 10
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    VarDeclStmt stmt(BindingKind::Immutable, Identifier{nameSpan}, nullptr, std::move(initializer), fullSpan);

    KAI_CHECK(stmt.kind() == StmtKind::VarDecl);
    KAI_CHECK(stmt.binding() == BindingKind::Immutable);
    KAI_CHECK(stmt.name().span == nameSpan);
    KAI_CHECK(sm.text(stmt.name().span) == "x");
    KAI_CHECK(stmt.type() == nullptr);
    KAI_CHECK(stmt.initializer().kind() == ExprKind::Literal);
    KAI_CHECK(stmt.initializer().span() == initSpan);
    KAI_CHECK(stmt.span() == fullSpan);
    KAI_CHECK(initializer == nullptr);
}

void testVarDeclStmtImmutableWithType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x: i32 = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan typeNameSpan = spanOf(file, 7, 10); // i32
    const SourceSpan initSpan = spanOf(file, 13, 15);    // 10
    const SourceSpan fullSpan = spanOf(file, 0, 15);

    TypeSyntaxPtr type = std::make_unique<NamedTypeSyntax>(Identifier{typeNameSpan}, typeNameSpan);
    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    VarDeclStmt stmt(BindingKind::Immutable, Identifier{nameSpan}, std::move(type), std::move(initializer), fullSpan);

    KAI_CHECK(stmt.binding() == BindingKind::Immutable);
    KAI_CHECK(stmt.type() != nullptr);
    KAI_CHECK(sm.text(stmt.type()->span()) == "i32");
    KAI_CHECK(stmt.initializer().span() == initSpan);
    KAI_CHECK(stmt.span() == fullSpan);
    KAI_CHECK(type == nullptr);
    KAI_CHECK(initializer == nullptr);
}

void testVarDeclStmtMutableWithoutType() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "mut x = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan initSpan = spanOf(file, 8, 10);
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    VarDeclStmt stmt(BindingKind::Mutable, Identifier{nameSpan}, nullptr, std::move(initializer), fullSpan);

    KAI_CHECK(stmt.binding() == BindingKind::Mutable);
    KAI_CHECK(stmt.type() == nullptr);
    KAI_CHECK(sm.text(stmt.name().span) == "x");
}

void testVarDeclStmtMutableWithType() {
    // "mut total: f64 = 0.0"
    //  0123456789012345678901
    //            1111111111 2
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "mut total: f64 = 0.0");
    const SourceSpan nameSpan = spanOf(file, 4, 9);       // total
    const SourceSpan typeNameSpan = spanOf(file, 11, 14); // f64
    const SourceSpan initSpan = spanOf(file, 17, 20);     // 0.0
    const SourceSpan fullSpan = spanOf(file, 0, 20);

    TypeSyntaxPtr type = std::make_unique<NamedTypeSyntax>(Identifier{typeNameSpan}, typeNameSpan);
    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Float, initSpan);
    VarDeclStmt stmt(BindingKind::Mutable, Identifier{nameSpan}, std::move(type), std::move(initializer), fullSpan);

    KAI_CHECK(stmt.binding() == BindingKind::Mutable);
    KAI_CHECK(sm.text(stmt.name().span) == "total");
    KAI_CHECK(stmt.type() != nullptr);
    KAI_CHECK(sm.text(stmt.type()->span()) == "f64");
    KAI_CHECK(sm.text(stmt.initializer().span()) == "0.0");
    KAI_CHECK(stmt.span() == fullSpan);
}

// --- ReturnStmt ---

void testReturnStmtBare() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "return");
    const SourceSpan span = spanOf(file, 0, 6);

    ReturnStmt stmt(nullptr, span);

    KAI_CHECK(stmt.kind() == StmtKind::Return);
    KAI_CHECK(stmt.value() == nullptr);
    KAI_CHECK(stmt.span() == span);
}

void testReturnStmtValued() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "return 42");
    const SourceSpan valueSpan = spanOf(file, 7, 9); // 42
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    ExprPtr value = std::make_unique<LiteralExpr>(LiteralKind::Integer, valueSpan);
    ReturnStmt stmt(std::move(value), fullSpan);

    KAI_CHECK(stmt.kind() == StmtKind::Return);
    KAI_CHECK(stmt.value() != nullptr);
    KAI_CHECK(stmt.value()->span() == valueSpan);
    KAI_CHECK(sm.text(stmt.value()->span()) == "42");
    KAI_CHECK(stmt.span() == fullSpan);
    KAI_CHECK(value == nullptr);
}

} // namespace

int main() {
    testExprStmtOwnsExpression();
    testBlockStmtOwnsAndMovesStatementList();
    testBlockStmtSupportsEmptyBody();

    testVarDeclStmtImmutableWithoutType();
    testVarDeclStmtImmutableWithType();
    testVarDeclStmtMutableWithoutType();
    testVarDeclStmtMutableWithType();

    testReturnStmtBare();
    testReturnStmtValued();

    return kai::test::failureCount == 0 ? 0 : 1;
}
