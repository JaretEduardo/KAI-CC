#include "kai/ast/Expr.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::BinaryOperator;
using kai::ast::CallExpr;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::ParenExpr;
using kai::ast::UnaryExpr;
using kai::ast::UnaryOperator;

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

// --- UnaryExpr ---

void testUnaryExprNegate() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "-x");
    const SourceSpan opSpan = spanOf(file, 0, 1);      // -
    const SourceSpan operandSpan = spanOf(file, 1, 2); // x
    const SourceSpan fullSpan = spanOf(file, 0, 2);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    UnaryExpr expr(UnaryOperator::Negate, opSpan, std::move(operand), fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::Unary);
    KAI_CHECK(expr.op() == UnaryOperator::Negate);
    KAI_CHECK(expr.operatorSpan() == opSpan);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "-");
    KAI_CHECK(expr.operand().kind() == ExprKind::Identifier);
    KAI_CHECK(expr.operand().span() == operandSpan);
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(operand == nullptr);
}

void testUnaryExprNot() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "!x");
    const SourceSpan opSpan = spanOf(file, 0, 1);
    const SourceSpan operandSpan = spanOf(file, 1, 2);
    const SourceSpan fullSpan = spanOf(file, 0, 2);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    UnaryExpr expr(UnaryOperator::Not, opSpan, std::move(operand), fullSpan);

    KAI_CHECK(expr.op() == UnaryOperator::Not);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "!");
    KAI_CHECK(expr.span() == fullSpan);
}

void testUnaryExprRef() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&x");
    const SourceSpan opSpan = spanOf(file, 0, 1);
    const SourceSpan operandSpan = spanOf(file, 1, 2);
    const SourceSpan fullSpan = spanOf(file, 0, 2);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    UnaryExpr expr(UnaryOperator::Ref, opSpan, std::move(operand), fullSpan);

    KAI_CHECK(expr.op() == UnaryOperator::Ref);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "&");
    KAI_CHECK(expr.span() == fullSpan);
}

void testUnaryExprRefMut() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&mut x");
    const SourceSpan opSpan = spanOf(file, 0, 4);      // &mut
    const SourceSpan operandSpan = spanOf(file, 5, 6); // x
    const SourceSpan fullSpan = spanOf(file, 0, 6);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    UnaryExpr expr(UnaryOperator::RefMut, opSpan, std::move(operand), fullSpan);

    KAI_CHECK(expr.op() == UnaryOperator::RefMut);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "&mut");
    KAI_CHECK(expr.span() == fullSpan);
}

void testUnaryExprRefMutOperatorSpanMayIncludeWhitespace() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "& mut x");
    const SourceSpan opSpan = spanOf(file, 0, 5);      // "& mut"
    const SourceSpan operandSpan = spanOf(file, 6, 7); // x
    const SourceSpan fullSpan = spanOf(file, 0, 7);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    UnaryExpr expr(UnaryOperator::RefMut, opSpan, std::move(operand), fullSpan);

    // A multi-token operator span (Amp.begin -> KwMut.end) may legitimately
    // include source whitespace between the two tokens; no meaning is
    // attached to it.
    KAI_CHECK(expr.op() == UnaryOperator::RefMut);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "& mut");
    KAI_CHECK(expr.span() == fullSpan);
}

void testUnaryExprNestedOperandOwnership() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "--x");
    const SourceSpan outerOpSpan = spanOf(file, 0, 1);
    const SourceSpan innerOpSpan = spanOf(file, 1, 2);
    const SourceSpan operandSpan = spanOf(file, 2, 3);
    const SourceSpan innerSpan = spanOf(file, 1, 3);
    const SourceSpan outerSpan = spanOf(file, 0, 3);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    ExprPtr inner = std::make_unique<UnaryExpr>(UnaryOperator::Negate, innerOpSpan, std::move(operand), innerSpan);
    UnaryExpr outer(UnaryOperator::Negate, outerOpSpan, std::move(inner), outerSpan);

    KAI_CHECK(outer.operand().kind() == ExprKind::Unary);
    const auto& innerExpr = static_cast<const UnaryExpr&>(outer.operand());
    KAI_CHECK(innerExpr.op() == UnaryOperator::Negate);
    KAI_CHECK(innerExpr.operand().kind() == ExprKind::Identifier);
    KAI_CHECK(inner == nullptr);
}

// --- BinaryExpr ---

void checkBinaryOperator(BinaryOperator op, const std::string& symbol) {
    SourceManager sm;
    const std::string source = "a" + symbol + "b";
    const FileId file = sm.addVirtualFile("a.kai", source);

    const auto symbolLen = static_cast<std::uint32_t>(symbol.size());
    const SourceSpan leftSpan = spanOf(file, 0, 1);
    const SourceSpan opSpan = spanOf(file, 1, 1 + symbolLen);
    const SourceSpan rightSpan = spanOf(file, 1 + symbolLen, 2 + symbolLen);
    const SourceSpan fullSpan = spanOf(file, 0, 2 + symbolLen);

    ExprPtr left = std::make_unique<IdentifierExpr>(Identifier{leftSpan}, leftSpan);
    ExprPtr right = std::make_unique<IdentifierExpr>(Identifier{rightSpan}, rightSpan);
    BinaryExpr expr(op, opSpan, std::move(left), std::move(right), fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::Binary);
    KAI_CHECK(expr.op() == op);
    KAI_CHECK(expr.operatorSpan() == opSpan);
    KAI_CHECK(sm.text(expr.operatorSpan()) == symbol);
    KAI_CHECK(expr.left().span() == leftSpan);
    KAI_CHECK(expr.right().span() == rightSpan);
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(left == nullptr);
    KAI_CHECK(right == nullptr);
}

void testBinaryExprAllOperators() {
    checkBinaryOperator(BinaryOperator::Or, "||");
    checkBinaryOperator(BinaryOperator::And, "&&");
    checkBinaryOperator(BinaryOperator::Equal, "==");
    checkBinaryOperator(BinaryOperator::NotEqual, "!=");
    checkBinaryOperator(BinaryOperator::Less, "<");
    checkBinaryOperator(BinaryOperator::LessEqual, "<=");
    checkBinaryOperator(BinaryOperator::Greater, ">");
    checkBinaryOperator(BinaryOperator::GreaterEqual, ">=");
    checkBinaryOperator(BinaryOperator::Range, "..");
    checkBinaryOperator(BinaryOperator::Add, "+");
    checkBinaryOperator(BinaryOperator::Subtract, "-");
    checkBinaryOperator(BinaryOperator::Multiply, "*");
    checkBinaryOperator(BinaryOperator::Divide, "/");
    checkBinaryOperator(BinaryOperator::Modulo, "%");
}

void testBinaryExprNestedLeftAssociativeShape() {
    // Hand-built shape for `1 + 2 * 3` -> Add(1, Multiply(2, 3)), the
    // precedence tree the future Parser is expected to build.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "1+2*3");

    const SourceSpan oneSpan = spanOf(file, 0, 1);
    const SourceSpan plusSpan = spanOf(file, 1, 2);
    const SourceSpan twoSpan = spanOf(file, 2, 3);
    const SourceSpan starSpan = spanOf(file, 3, 4);
    const SourceSpan threeSpan = spanOf(file, 4, 5);
    const SourceSpan mulSpan = spanOf(file, 2, 5);
    const SourceSpan addSpan = spanOf(file, 0, 5);

    ExprPtr two = std::make_unique<LiteralExpr>(LiteralKind::Integer, twoSpan);
    ExprPtr three = std::make_unique<LiteralExpr>(LiteralKind::Integer, threeSpan);
    ExprPtr mul = std::make_unique<BinaryExpr>(BinaryOperator::Multiply, starSpan, std::move(two), std::move(three),
                                                mulSpan);

    ExprPtr one = std::make_unique<LiteralExpr>(LiteralKind::Integer, oneSpan);
    BinaryExpr add(BinaryOperator::Add, plusSpan, std::move(one), std::move(mul), addSpan);

    KAI_CHECK(add.op() == BinaryOperator::Add);
    KAI_CHECK(add.left().kind() == ExprKind::Literal);
    KAI_CHECK(add.right().kind() == ExprKind::Binary);
    const auto& rightMul = static_cast<const BinaryExpr&>(add.right());
    KAI_CHECK(rightMul.op() == BinaryOperator::Multiply);
}

// --- AssignmentExpr ---

void testAssignmentExprOwnsTargetAndValue() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "a = b");
    const SourceSpan targetSpan = spanOf(file, 0, 1);
    const SourceSpan opSpan = spanOf(file, 2, 3);
    const SourceSpan valueSpan = spanOf(file, 4, 5);
    const SourceSpan fullSpan = spanOf(file, 0, 5);

    ExprPtr target = std::make_unique<IdentifierExpr>(Identifier{targetSpan}, targetSpan);
    ExprPtr value = std::make_unique<IdentifierExpr>(Identifier{valueSpan}, valueSpan);
    AssignmentExpr expr(std::move(target), opSpan, std::move(value), fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::Assignment);
    KAI_CHECK(expr.operatorSpan() == opSpan);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "=");
    KAI_CHECK(expr.target().span() == targetSpan);
    KAI_CHECK(expr.value().span() == valueSpan);
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(target == nullptr);
    KAI_CHECK(value == nullptr);
}

void testAssignmentExprRightAssociativeChainShape() {
    // Hand-built shape for `a = b = c` -> Assignment(a, Assignment(b, c)).
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "a=b=c");

    const SourceSpan aSpan = spanOf(file, 0, 1);
    const SourceSpan eq1Span = spanOf(file, 1, 2);
    const SourceSpan bSpan = spanOf(file, 2, 3);
    const SourceSpan eq2Span = spanOf(file, 3, 4);
    const SourceSpan cSpan = spanOf(file, 4, 5);
    const SourceSpan innerSpan = spanOf(file, 2, 5);
    const SourceSpan outerSpan = spanOf(file, 0, 5);

    ExprPtr b = std::make_unique<IdentifierExpr>(Identifier{bSpan}, bSpan);
    ExprPtr c = std::make_unique<IdentifierExpr>(Identifier{cSpan}, cSpan);
    ExprPtr inner = std::make_unique<AssignmentExpr>(std::move(b), eq2Span, std::move(c), innerSpan);

    ExprPtr a = std::make_unique<IdentifierExpr>(Identifier{aSpan}, aSpan);
    AssignmentExpr outer(std::move(a), eq1Span, std::move(inner), outerSpan);

    KAI_CHECK(outer.target().kind() == ExprKind::Identifier);
    KAI_CHECK(outer.value().kind() == ExprKind::Assignment);
    const auto& innerAssign = static_cast<const AssignmentExpr&>(outer.value());
    KAI_CHECK(innerAssign.target().kind() == ExprKind::Identifier);
    KAI_CHECK(innerAssign.value().kind() == ExprKind::Identifier);
}

void testAssignmentExprTargetNotRestrictedToIdentifier() {
    // The AST places no lvalue restriction on target: a LiteralExpr target
    // constructs successfully. Rejecting this belongs to semantic
    // analysis, not the AST or the parser.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "1 = 2");
    const SourceSpan targetSpan = spanOf(file, 0, 1);
    const SourceSpan opSpan = spanOf(file, 2, 3);
    const SourceSpan valueSpan = spanOf(file, 4, 5);
    const SourceSpan fullSpan = spanOf(file, 0, 5);

    ExprPtr target = std::make_unique<LiteralExpr>(LiteralKind::Integer, targetSpan);
    ExprPtr value = std::make_unique<LiteralExpr>(LiteralKind::Integer, valueSpan);
    AssignmentExpr expr(std::move(target), opSpan, std::move(value), fullSpan);

    KAI_CHECK(expr.target().kind() == ExprKind::Literal);
    KAI_CHECK(expr.value().kind() == ExprKind::Literal);
}

} // namespace

int main() {
    testLiteralExprConstruction();
    testIdentifierExprConstruction();
    testCallExprOwnsAndMovesCalleeAndArguments();
    testCallExprSupportsEmptyArgumentList();
    testParenExprOwnsInnerAndPreservesDistinctOuterSpan();

    testUnaryExprNegate();
    testUnaryExprNot();
    testUnaryExprRef();
    testUnaryExprRefMut();
    testUnaryExprRefMutOperatorSpanMayIncludeWhitespace();
    testUnaryExprNestedOperandOwnership();

    testBinaryExprAllOperators();
    testBinaryExprNestedLeftAssociativeShape();

    testAssignmentExprOwnsTargetAndValue();
    testAssignmentExprRightAssociativeChainShape();
    testAssignmentExprTargetNotRestrictedToIdentifier();

    return kai::test::failureCount == 0 ? 0 : 1;
}
