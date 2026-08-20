#include "kai/ast/Expr.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::ArrayLiteralExpr;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::BinaryOperator;
using kai::ast::CallExpr;
using kai::ast::ErrorPropagationExpr;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::IndexExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::MemberExpr;
using kai::ast::ParenExpr;
using kai::ast::UnaryExpr;
using kai::ast::UnaryOperator;
using kai::ast::UnitExpr;

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

// --- UnitExpr ---

void testUnitExprConstruction() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "()");
    const SourceSpan span = spanOf(file, 0, 2);

    const UnitExpr expr(span);

    KAI_CHECK(expr.kind() == ExprKind::Unit);
    KAI_CHECK(expr.span() == span);
    KAI_CHECK(sm.text(expr.span()) == "()");
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

// --- ArrayLiteralExpr ---

void testArrayLiteralExprEmpty() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[]");
    const SourceSpan span = spanOf(file, 0, 2);

    ArrayLiteralExpr expr(std::vector<ExprPtr>{}, span);

    KAI_CHECK(expr.kind() == ExprKind::ArrayLiteral);
    KAI_CHECK(expr.elements().empty());
    KAI_CHECK(expr.span() == span);
}

void testArrayLiteralExprNonEmptyOrderAndOwnership() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[1, 2, 3]");
    const SourceSpan oneSpan = spanOf(file, 1, 2);
    const SourceSpan twoSpan = spanOf(file, 4, 5);
    const SourceSpan threeSpan = spanOf(file, 7, 8);
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    std::vector<ExprPtr> elements;
    elements.push_back(std::make_unique<LiteralExpr>(LiteralKind::Integer, oneSpan));
    elements.push_back(std::make_unique<LiteralExpr>(LiteralKind::Integer, twoSpan));
    elements.push_back(std::make_unique<LiteralExpr>(LiteralKind::Integer, threeSpan));

    ArrayLiteralExpr expr(std::move(elements), fullSpan);

    KAI_CHECK(expr.elements().size() == 3);
    KAI_CHECK(sm.text(expr.elements()[0]->span()) == "1");
    KAI_CHECK(sm.text(expr.elements()[1]->span()) == "2");
    KAI_CHECK(sm.text(expr.elements()[2]->span()) == "3");
    KAI_CHECK(expr.span() == fullSpan);

    // Ownership actually transferred out of the local vector.
    KAI_CHECK(elements.empty());
}

// --- IndexExpr ---

void testIndexExprBasic() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "values[0]");
    const SourceSpan objectSpan = spanOf(file, 0, 6); // values
    const SourceSpan indexSpan = spanOf(file, 7, 8);  // 0
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    ExprPtr object = std::make_unique<IdentifierExpr>(Identifier{objectSpan}, objectSpan);
    ExprPtr index = std::make_unique<LiteralExpr>(LiteralKind::Integer, indexSpan);
    IndexExpr expr(std::move(object), std::move(index), fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::Index);
    KAI_CHECK(expr.object().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(expr.object().span()) == "values");
    KAI_CHECK(expr.index().kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(expr.index().span()) == "0");
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(object == nullptr);
    KAI_CHECK(index == nullptr);
}

void testIndexExprArbitraryIndexExpression() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "values[i + 1]");
    const SourceSpan objectSpan = spanOf(file, 0, 6);
    const SourceSpan iSpan = spanOf(file, 7, 8);
    const SourceSpan plusSpan = spanOf(file, 9, 10);
    const SourceSpan oneSpan = spanOf(file, 11, 12);
    const SourceSpan addSpan = spanOf(file, 7, 12);
    const SourceSpan fullSpan = spanOf(file, 0, 13);

    ExprPtr object = std::make_unique<IdentifierExpr>(Identifier{objectSpan}, objectSpan);
    ExprPtr i = std::make_unique<IdentifierExpr>(Identifier{iSpan}, iSpan);
    ExprPtr one = std::make_unique<LiteralExpr>(LiteralKind::Integer, oneSpan);
    ExprPtr index = std::make_unique<BinaryExpr>(BinaryOperator::Add, plusSpan, std::move(i), std::move(one), addSpan);

    IndexExpr expr(std::move(object), std::move(index), fullSpan);

    KAI_CHECK(expr.index().kind() == ExprKind::Binary);
    KAI_CHECK(static_cast<const BinaryExpr&>(expr.index()).op() == BinaryOperator::Add);
    KAI_CHECK(expr.span() == fullSpan);
}

void testIndexExprNestedMatrixShape() {
    // matrix[i][j] -> IndexExpr(IndexExpr(matrix, i), j)
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "matrix[i][j]");
    const SourceSpan matrixSpan = spanOf(file, 0, 6); // matrix
    const SourceSpan iSpan = spanOf(file, 7, 8);      // i
    const SourceSpan innerSpan = spanOf(file, 0, 9);  // matrix[i]
    const SourceSpan jSpan = spanOf(file, 10, 11);    // j
    const SourceSpan outerSpan = spanOf(file, 0, 12); // matrix[i][j]

    ExprPtr matrix = std::make_unique<IdentifierExpr>(Identifier{matrixSpan}, matrixSpan);
    ExprPtr i = std::make_unique<IdentifierExpr>(Identifier{iSpan}, iSpan);
    ExprPtr inner = std::make_unique<IndexExpr>(std::move(matrix), std::move(i), innerSpan);

    ExprPtr j = std::make_unique<IdentifierExpr>(Identifier{jSpan}, jSpan);
    IndexExpr outer(std::move(inner), std::move(j), outerSpan);

    KAI_CHECK(outer.object().kind() == ExprKind::Index);
    const auto& innerIndex = static_cast<const IndexExpr&>(outer.object());
    KAI_CHECK(innerIndex.object().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(innerIndex.object().span()) == "matrix");
    KAI_CHECK(sm.text(outer.index().span()) == "j");
    KAI_CHECK(outer.span() == outerSpan);
}

// --- MemberExpr ---

void testMemberExprBasic() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "values.len");
    const SourceSpan objectSpan = spanOf(file, 0, 6); // values
    const SourceSpan memberSpan = spanOf(file, 7, 10); // len
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr object = std::make_unique<IdentifierExpr>(Identifier{objectSpan}, objectSpan);
    MemberExpr expr(std::move(object), Identifier{memberSpan}, fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::Member);
    KAI_CHECK(expr.object().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(expr.object().span()) == "values");
    KAI_CHECK(expr.member().span == memberSpan);
    KAI_CHECK(sm.text(expr.member().span) == "len");
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(object == nullptr);
}

void testMemberExprChainedShape() {
    // object.a.b -> MemberExpr(MemberExpr(object, a), b)
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "object.a.b");
    const SourceSpan objectSpan = spanOf(file, 0, 6); // object
    const SourceSpan aSpan = spanOf(file, 7, 8);      // a
    const SourceSpan innerSpan = spanOf(file, 0, 8);  // object.a
    const SourceSpan bSpan = spanOf(file, 9, 10);     // b
    const SourceSpan outerSpan = spanOf(file, 0, 10); // object.a.b

    ExprPtr object = std::make_unique<IdentifierExpr>(Identifier{objectSpan}, objectSpan);
    ExprPtr inner = std::make_unique<MemberExpr>(std::move(object), Identifier{aSpan}, innerSpan);
    MemberExpr outer(std::move(inner), Identifier{bSpan}, outerSpan);

    KAI_CHECK(outer.object().kind() == ExprKind::Member);
    const auto& innerMember = static_cast<const MemberExpr&>(outer.object());
    KAI_CHECK(sm.text(innerMember.member().span) == "a");
    KAI_CHECK(innerMember.object().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(outer.member().span) == "b");
    KAI_CHECK(outer.span() == outerSpan);
}

// --- ErrorPropagationExpr ---

void testErrorPropagationExprBasic() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "value?");
    const SourceSpan operandSpan = spanOf(file, 0, 5);  // value
    const SourceSpan questionSpan = spanOf(file, 5, 6); // ?
    const SourceSpan fullSpan = spanOf(file, 0, 6);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    ErrorPropagationExpr expr(std::move(operand), questionSpan, fullSpan);

    KAI_CHECK(expr.kind() == ExprKind::ErrorPropagation);
    KAI_CHECK(expr.operand().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(expr.operand().span()) == "value");
    KAI_CHECK(expr.operatorSpan() == questionSpan);
    KAI_CHECK(sm.text(expr.operatorSpan()) == "?");
    KAI_CHECK(expr.span() == fullSpan);
    KAI_CHECK(operand == nullptr);
}

void testErrorPropagationExprWrapsCall() {
    // load()? -> ErrorPropagationExpr(CallExpr(load))
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "load()?");
    const SourceSpan calleeSpan = spanOf(file, 0, 4);   // load
    const SourceSpan callSpan = spanOf(file, 0, 6);     // load()
    const SourceSpan questionSpan = spanOf(file, 6, 7); // ?
    const SourceSpan fullSpan = spanOf(file, 0, 7);

    ExprPtr callee = std::make_unique<IdentifierExpr>(Identifier{calleeSpan}, calleeSpan);
    ExprPtr call = std::make_unique<CallExpr>(std::move(callee), std::vector<ExprPtr>{}, callSpan);
    ErrorPropagationExpr expr(std::move(call), questionSpan, fullSpan);

    KAI_CHECK(expr.operand().kind() == ExprKind::Call);
    KAI_CHECK(expr.span() == fullSpan);
}

void testErrorPropagationExprNestedEachOwnsDistinctOperatorSpan() {
    // value?? -> ErrorPropagationExpr(ErrorPropagationExpr(value))
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "value??");
    const SourceSpan operandSpan = spanOf(file, 0, 5);   // value
    const SourceSpan innerQuestion = spanOf(file, 5, 6); // first ?
    const SourceSpan innerSpan = spanOf(file, 0, 6);     // value?
    const SourceSpan outerQuestion = spanOf(file, 6, 7); // second ?
    const SourceSpan outerSpan = spanOf(file, 0, 7);     // value??

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    ExprPtr inner = std::make_unique<ErrorPropagationExpr>(std::move(operand), innerQuestion, innerSpan);
    ErrorPropagationExpr outer(std::move(inner), outerQuestion, outerSpan);

    KAI_CHECK(outer.kind() == ExprKind::ErrorPropagation);
    KAI_CHECK(outer.operatorSpan() == outerQuestion);
    KAI_CHECK(outer.operand().kind() == ExprKind::ErrorPropagation);

    const auto& innerExpr = static_cast<const ErrorPropagationExpr&>(outer.operand());
    KAI_CHECK(innerExpr.operatorSpan() == innerQuestion);
    KAI_CHECK(innerExpr.operand().kind() == ExprKind::Identifier);

    // The two operatorSpans are distinct, each pinpointing its own `?`.
    KAI_CHECK(outer.operatorSpan() != innerExpr.operatorSpan());
    KAI_CHECK(outer.span() == outerSpan);
}

} // namespace

int main() {
    testLiteralExprConstruction();
    testIdentifierExprConstruction();
    testCallExprOwnsAndMovesCalleeAndArguments();
    testCallExprSupportsEmptyArgumentList();
    testParenExprOwnsInnerAndPreservesDistinctOuterSpan();

    testUnitExprConstruction();

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

    testArrayLiteralExprEmpty();
    testArrayLiteralExprNonEmptyOrderAndOwnership();

    testIndexExprBasic();
    testIndexExprArbitraryIndexExpression();
    testIndexExprNestedMatrixShape();

    testMemberExprBasic();
    testMemberExprChainedShape();

    testErrorPropagationExprBasic();
    testErrorPropagationExprWrapsCall();
    testErrorPropagationExprNestedEachOwnsDistinctOperatorSpan();

    return kai::test::failureCount == 0 ? 0 : 1;
}
