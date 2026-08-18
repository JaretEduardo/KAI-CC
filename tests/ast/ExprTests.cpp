#include "kai/ast/Expr.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::CallExpr;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::ParenExpr;

namespace {

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

void testLiteralExprConstruction() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "\"Hello from KAI\"");
    const SourceSpan span = spanOf(file, 0, sm.buffer(file).size());

    const LiteralExpr literal(LiteralKind::String, span);

    KAI_CHECK(literal.kind() == ExprKind::Literal);
    KAI_CHECK(literal.literalKind() == LiteralKind::String);
    KAI_CHECK(literal.span() == span);
    KAI_CHECK(sm.text(literal.span()) == "\"Hello from KAI\"");
}

void testIdentifierExprConstruction() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "print");
    const SourceSpan span = spanOf(file, 0, 5);

    const IdentifierExpr expr(Identifier{span}, span);

    KAI_CHECK(expr.kind() == ExprKind::Identifier);
    KAI_CHECK(expr.name().span == span);
    KAI_CHECK(sm.text(expr.name().span) == "print");
}

void testCallExprOwnsAndMovesCalleeAndArguments() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "print(\"Hello from KAI\")");

    const SourceSpan calleeSpan = spanOf(file, 0, 5);         // print
    const SourceSpan argSpan = spanOf(file, 6, 22);           // "Hello from KAI"
    const SourceSpan callSpan = spanOf(file, 0, 23);          // print(...)

    ExprPtr callee = std::make_unique<IdentifierExpr>(Identifier{calleeSpan}, calleeSpan);

    std::vector<ExprPtr> arguments;
    arguments.push_back(std::make_unique<LiteralExpr>(LiteralKind::String, argSpan));

    CallExpr call(std::move(callee), std::move(arguments), callSpan);

    KAI_CHECK(call.kind() == ExprKind::Call);
    KAI_CHECK(call.span() == callSpan);

    KAI_CHECK(call.callee().kind() == ExprKind::Identifier);
    KAI_CHECK(call.callee().span() == calleeSpan);

    KAI_CHECK(call.arguments().size() == 1);
    KAI_CHECK(call.arguments()[0]->kind() == ExprKind::Literal);
    KAI_CHECK(call.arguments()[0]->span() == argSpan);

    // Ownership actually transferred: the local `callee`/`arguments`
    // were moved-from, not copied.
    KAI_CHECK(callee == nullptr);
    KAI_CHECK(arguments.empty());
}

void testCallExprSupportsEmptyArgumentList() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "main()");
    const SourceSpan calleeSpan = spanOf(file, 0, 4);
    const SourceSpan callSpan = spanOf(file, 0, 6);

    ExprPtr callee = std::make_unique<IdentifierExpr>(Identifier{calleeSpan}, calleeSpan);
    CallExpr call(std::move(callee), std::vector<ExprPtr>{}, callSpan);

    KAI_CHECK(call.arguments().empty());
}

void testParenExprOwnsInnerAndPreservesDistinctOuterSpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "(42)");

    const SourceSpan innerSpan = spanOf(file, 1, 3); // 42
    const SourceSpan outerSpan = spanOf(file, 0, 4); // (42)

    ExprPtr inner = std::make_unique<LiteralExpr>(LiteralKind::Integer, innerSpan);
    ParenExpr paren(std::move(inner), outerSpan);

    KAI_CHECK(paren.kind() == ExprKind::Paren);
    KAI_CHECK(paren.span() == outerSpan);
    KAI_CHECK(sm.text(paren.span()) == "(42)");

    KAI_CHECK(paren.inner().kind() == ExprKind::Literal);
    KAI_CHECK(paren.inner().span() == innerSpan);
    KAI_CHECK(sm.text(paren.inner().span()) == "42");

    // The outer and inner spans must remain distinct - ParenExpr does
    // not widen the child's own span to match its own.
    KAI_CHECK(paren.span() != paren.inner().span());

    KAI_CHECK(inner == nullptr);
}

} // namespace

int main() {
    testLiteralExprConstruction();
    testIdentifierExprConstruction();
    testCallExprOwnsAndMovesCalleeAndArguments();
    testCallExprSupportsEmptyArgumentList();
    testParenExprOwnsInnerAndPreservesDistinctOuterSpan();

    return kai::test::failureCount == 0 ? 0 : 1;
}
