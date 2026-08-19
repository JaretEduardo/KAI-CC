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
using kai::ast::BinaryExpr;
using kai::ast::BinaryOperator;
using kai::ast::BindingKind;
using kai::ast::BlockPtr;
using kai::ast::BlockStmt;
using kai::ast::CallExpr;
using kai::ast::ElseClause;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::ExprStmt;
using kai::ast::ForStmt;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::IfBranch;
using kai::ast::IfStmt;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::NamedTypeSyntax;
using kai::ast::ReturnStmt;
using kai::ast::StmtKind;
using kai::ast::StmtPtr;
using kai::ast::TypeSyntaxPtr;
using kai::ast::VarDeclStmt;
using kai::ast::WhileStmt;

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

// --- IfStmt / IfBranch / ElseClause ---

BlockPtr emptyBlock(SourceSpan span) { return std::make_unique<BlockStmt>(std::vector<StmtPtr>{}, span); }

void testIfStmtOneBranchNoElse() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "if a {}");
    const SourceSpan condSpan = spanOf(file, 3, 4); // a
    const SourceSpan bodySpan = spanOf(file, 5, 7); // {}
    const SourceSpan branchSpan = spanOf(file, 0, 7);

    std::vector<IfBranch> branches;
    branches.push_back(IfBranch{std::make_unique<IdentifierExpr>(Identifier{condSpan}, condSpan),
                                 emptyBlock(bodySpan), branchSpan});

    IfStmt stmt(std::move(branches), std::nullopt, branchSpan);

    KAI_CHECK(stmt.kind() == StmtKind::If);
    KAI_CHECK(stmt.branches().size() == 1);
    KAI_CHECK(!stmt.elseClause().has_value());
    KAI_CHECK(stmt.branches()[0].span == branchSpan);
    KAI_CHECK(sm.text(stmt.branches()[0].span) == "if a {}");
    KAI_CHECK(stmt.branches()[0].condition->kind() == ExprKind::Identifier);
    KAI_CHECK(stmt.branches()[0].body->kind() == StmtKind::Block);
    KAI_CHECK(stmt.span() == branchSpan);
}

void testIfStmtWithElseClause() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "if a {} else {}");
    const SourceSpan condSpan = spanOf(file, 3, 4);   // a
    const SourceSpan bodySpan = spanOf(file, 5, 7);   // {}
    const SourceSpan branchSpan = spanOf(file, 0, 7); // if a {}
    const SourceSpan elseBodySpan = spanOf(file, 13, 15); // {}
    const SourceSpan elseSpan = spanOf(file, 8, 15);      // else {}
    const SourceSpan fullSpan = spanOf(file, 0, 15);

    std::vector<IfBranch> branches;
    branches.push_back(IfBranch{std::make_unique<IdentifierExpr>(Identifier{condSpan}, condSpan),
                                 emptyBlock(bodySpan), branchSpan});

    ElseClause elseClause{emptyBlock(elseBodySpan), elseSpan};
    IfStmt stmt(std::move(branches), std::move(elseClause), fullSpan);

    KAI_CHECK(stmt.elseClause().has_value());
    KAI_CHECK(stmt.elseClause()->span == elseSpan);
    KAI_CHECK(sm.text(stmt.elseClause()->span) == "else {}");
    KAI_CHECK(stmt.elseClause()->body->kind() == StmtKind::Block);
    KAI_CHECK(stmt.span() == fullSpan);
}

void testIfStmtMultipleElseIfBranches() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "if a {} else if b {} else if c {} else {}");

    const SourceSpan cond0 = spanOf(file, 3, 4);   // a
    const SourceSpan body0 = spanOf(file, 5, 7);   // {}
    const SourceSpan branch0 = spanOf(file, 0, 7); // if a {}

    const SourceSpan cond1 = spanOf(file, 16, 17);  // b
    const SourceSpan body1 = spanOf(file, 18, 20);  // {}
    const SourceSpan branch1 = spanOf(file, 8, 20); // else if b {}

    const SourceSpan cond2 = spanOf(file, 29, 30);   // c
    const SourceSpan body2 = spanOf(file, 31, 33);   // {}
    const SourceSpan branch2 = spanOf(file, 21, 33); // else if c {}

    const SourceSpan elseBody = spanOf(file, 39, 41); // {}
    const SourceSpan elseSpan = spanOf(file, 34, 41); // else {}
    const SourceSpan fullSpan = spanOf(file, 0, 41);

    std::vector<IfBranch> branches;
    branches.push_back(
        IfBranch{std::make_unique<IdentifierExpr>(Identifier{cond0}, cond0), emptyBlock(body0), branch0});
    branches.push_back(
        IfBranch{std::make_unique<IdentifierExpr>(Identifier{cond1}, cond1), emptyBlock(body1), branch1});
    branches.push_back(
        IfBranch{std::make_unique<IdentifierExpr>(Identifier{cond2}, cond2), emptyBlock(body2), branch2});

    ElseClause elseClause{emptyBlock(elseBody), elseSpan};
    IfStmt stmt(std::move(branches), std::move(elseClause), fullSpan);

    KAI_CHECK(stmt.branches().size() == 3);

    // Branch order and identity: each branch's condition is distinct and
    // in source order (branches[0]=a, [1]=b, [2]=c) - not interchangeable.
    KAI_CHECK(sm.text(stmt.branches()[0].condition->span()) == "a");
    KAI_CHECK(sm.text(stmt.branches()[1].condition->span()) == "b");
    KAI_CHECK(sm.text(stmt.branches()[2].condition->span()) == "c");

    // Initial branch span begins at `if`.
    KAI_CHECK(sm.text(stmt.branches()[0].span) == "if a {}");
    // else-if branch spans begin at `else`, not the following `if`.
    KAI_CHECK(sm.text(stmt.branches()[1].span) == "else if b {}");
    KAI_CHECK(sm.text(stmt.branches()[2].span) == "else if c {}");

    KAI_CHECK(stmt.elseClause().has_value());
    KAI_CHECK(sm.text(stmt.elseClause()->span) == "else {}");

    KAI_CHECK(stmt.span() == fullSpan);
}

// --- WhileStmt ---

void testWhileStmtConditionBodySpan() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "while a {}");
    const SourceSpan condSpan = spanOf(file, 6, 7); // a
    const SourceSpan bodySpan = spanOf(file, 8, 10); // {}
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr condition = std::make_unique<IdentifierExpr>(Identifier{condSpan}, condSpan);
    WhileStmt stmt(std::move(condition), emptyBlock(bodySpan), fullSpan);

    KAI_CHECK(stmt.kind() == StmtKind::While);
    KAI_CHECK(stmt.condition().span() == condSpan);
    KAI_CHECK(stmt.body().kind() == StmtKind::Block);
    KAI_CHECK(stmt.body().span() == bodySpan);
    KAI_CHECK(stmt.span() == fullSpan);
}

// --- ForStmt ---

void testForStmtWithIdentifierIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "for value in values {}");
    const SourceSpan varSpan = spanOf(file, 4, 9);       // value
    const SourceSpan iterableSpan = spanOf(file, 13, 19); // values
    const SourceSpan bodySpan = spanOf(file, 20, 22);     // {}
    const SourceSpan fullSpan = spanOf(file, 0, 22);

    ExprPtr iterable = std::make_unique<IdentifierExpr>(Identifier{iterableSpan}, iterableSpan);
    ForStmt stmt(Identifier{varSpan}, std::move(iterable), emptyBlock(bodySpan), fullSpan);

    KAI_CHECK(stmt.kind() == StmtKind::For);
    KAI_CHECK(sm.text(stmt.variable().span) == "value");
    KAI_CHECK(stmt.iterable().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(stmt.iterable().span()) == "values");
    KAI_CHECK(stmt.body().kind() == StmtKind::Block);
    KAI_CHECK(stmt.span() == fullSpan);
}

void testForStmtWithRangeIterable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "for i in 0..10 {}");
    const SourceSpan varSpan = spanOf(file, 4, 5);   // i
    const SourceSpan leftSpan = spanOf(file, 9, 10); // 0
    const SourceSpan opSpan = spanOf(file, 10, 12);  // ..
    const SourceSpan rightSpan = spanOf(file, 12, 14); // 10
    const SourceSpan rangeSpan = spanOf(file, 9, 14);
    const SourceSpan bodySpan = spanOf(file, 15, 17); // {}
    const SourceSpan fullSpan = spanOf(file, 0, 17);

    ExprPtr left = std::make_unique<LiteralExpr>(LiteralKind::Integer, leftSpan);
    ExprPtr right = std::make_unique<LiteralExpr>(LiteralKind::Integer, rightSpan);
    ExprPtr iterable =
        std::make_unique<BinaryExpr>(BinaryOperator::Range, opSpan, std::move(left), std::move(right), rangeSpan);

    ForStmt stmt(Identifier{varSpan}, std::move(iterable), emptyBlock(bodySpan), fullSpan);

    KAI_CHECK(sm.text(stmt.variable().span) == "i");
    KAI_CHECK(stmt.iterable().kind() == ExprKind::Binary);
    const auto& range = static_cast<const BinaryExpr&>(stmt.iterable());
    KAI_CHECK(range.op() == BinaryOperator::Range);
    KAI_CHECK(stmt.span() == fullSpan);
}

// --- Nested control-flow ownership ---

void testNestedControlFlowOwnershipAndDestruction() {
    // IfStmt -> body contains WhileStmt -> body contains ForStmt.
    // Not independently checkable via KAI_CHECK (no leak sanitizer wired
    // into this harness), but a broken RAII chain would show up as a
    // crash when this test binary runs.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "if a { while b { for c in d {} } }");
    const SourceSpan whole = spanOf(file, 0, static_cast<std::uint32_t>(sm.buffer(file).size()));

    ExprPtr forIterable = std::make_unique<IdentifierExpr>(Identifier{whole}, whole);
    StmtPtr forStmt = std::make_unique<ForStmt>(Identifier{whole}, std::move(forIterable), emptyBlock(whole), whole);

    std::vector<StmtPtr> whileBodyStmts;
    whileBodyStmts.push_back(std::move(forStmt));
    BlockPtr whileBody = std::make_unique<BlockStmt>(std::move(whileBodyStmts), whole);

    ExprPtr whileCondition = std::make_unique<IdentifierExpr>(Identifier{whole}, whole);
    StmtPtr whileStmt = std::make_unique<WhileStmt>(std::move(whileCondition), std::move(whileBody), whole);

    std::vector<StmtPtr> ifBodyStmts;
    ifBodyStmts.push_back(std::move(whileStmt));
    BlockPtr ifBody = std::make_unique<BlockStmt>(std::move(ifBodyStmts), whole);

    ExprPtr ifCondition = std::make_unique<IdentifierExpr>(Identifier{whole}, whole);
    std::vector<IfBranch> branches;
    branches.push_back(IfBranch{std::move(ifCondition), std::move(ifBody), whole});
    IfStmt ifStmt(std::move(branches), std::nullopt, whole);

    KAI_CHECK(ifStmt.branches()[0].body->statements()[0]->kind() == StmtKind::While);
    const auto& innerWhile = static_cast<const WhileStmt&>(*ifStmt.branches()[0].body->statements()[0]);
    KAI_CHECK(innerWhile.body().statements()[0]->kind() == StmtKind::For);

    // Ownership actually transferred out of every local.
    KAI_CHECK(branches.empty());
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

    testIfStmtOneBranchNoElse();
    testIfStmtWithElseClause();
    testIfStmtMultipleElseIfBranches();

    testWhileStmtConditionBodySpan();

    testForStmtWithIdentifierIterable();
    testForStmtWithRangeIterable();

    testNestedControlFlowOwnershipAndDestruction();

    return kai::test::failureCount == 0 ? 0 : 1;
}
