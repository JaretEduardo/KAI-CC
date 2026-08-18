#include "kai/ast/Stmt.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::BlockStmt;
using kai::ast::CallExpr;
using kai::ast::Expr;
using kai::ast::ExprPtr;
using kai::ast::ExprStmt;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::StmtKind;
using kai::ast::StmtPtr;

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

} // namespace

int main() {
    testExprStmtOwnsExpression();
    testBlockStmtOwnsAndMovesStatementList();
    testBlockStmtSupportsEmptyBody();

    return kai::test::failureCount == 0 ? 0 : 1;
}
