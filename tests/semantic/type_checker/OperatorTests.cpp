#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- Milestone 2: Unary ---

void testNegateSignedIntegerReturnsOperandType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32, y: i64) {\n    let a = -x\n    let b = -y\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i32());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::i64());
    }
}

void testNegateFloatReturnsOperandType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(z: f32) {\n    let c = -z\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto cId = result.model.declarationSymbol(declC.name());
    KAI_CHECK(cId.has_value());
    if (cId) {
        KAI_CHECK(result.model.symbol(*cId).type == Type::f32());
    }
}

void testNegateUnsignedIntegerInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: u8) {\n    let y = -x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

void testNegateBoolInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(flag: bool) {\n    let y = -flag\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }
}

void testNotBoolReturnsBool() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(flag: bool) {\n    let a = !flag\n    let b = !true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::boolean());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::boolean());
    }
}

void testNotIntegerInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {\n    let y = !x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testNegativeFloatLiteralRegression() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: f32 = -1.5\n    let y = -1.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto xType = result.model.typeOf(declX.initializer());
    KAI_CHECK(xType.has_value());
    if (xType) {
        KAI_CHECK(*xType == Type::f32());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::f64());
    }
}

// --- Milestone 2: Arithmetic ---

void testArithmeticSameTypeSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: i32, b: i32, c: u64, d: u64, e: f32, g: f32) {\n"
            "    let x = a + b\n    let y = c * d\n    let z = e / g\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& declZ = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);

    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    const auto zId = result.model.declarationSymbol(declZ.name());
    KAI_CHECK(xId.has_value() && yId.has_value() && zId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::u64());
    }
    if (zId) {
        KAI_CHECK(result.model.symbol(*zId).type == Type::f32());
    }
}

void testArithmeticMixedTypesInvalidBinaryOperands() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(a: i32, b: i64, c: f32) {\n    let x = a + b\n    let y = a + c\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(!error.actualType.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(xId.has_value() && yId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 2: Modulo ---

void testModuloIntegerSucceeds() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(a: i32, b: i32, c: u64, d: u64) {\n    let x = a % b\n    let y = c % d\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::u64());
    }
}

void testModuloFloatInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: f32, b: f32) {\n    let x = a % b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Ordering ---

void testOrderingNumericSucceedsBool() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: i32, b: i32, c: u64, d: u64, e: f32, g: f32) {\n"
            "    let x = a < b\n    let y = c >= d\n    let z = e > g\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }
}

void testOrderingRejectsInvalidOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: bool, b: bool, c: char, d: char, e: i32, g: i64, h: i32, i: f32) {\n"
            "    let w = a < b\n    let x = c < d\n    let y = e < g\n    let z = h < i\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 4);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Logical ---

void testLogicalBoolSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) {\n    let x = true && false\n    let y = a || b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::boolean());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::boolean());
    }
}

void testLogicalNonBoolInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i32, b: bool) {\n    let x = a && b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Equality ---

void testEqualitySameTypeSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(p: i32, q: i32, r: f64, s: f64, a: bool, b: bool, c: char, d: char) {\n"
            "    let w = p == q\n    let x = r != s\n    let y = a == b\n    let z = c != d\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }
}

void testEqualityRejectsCrossTypeAndUnit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i32, b: i64, c: f64, d: bool) {\n"
                                          "    let w = a == b\n    let x = a == c\n"
                                          "    let y = d == a\n    let z = () == ()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 4);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: contextual arithmetic typing ---

void testWholeExpressionArithmeticContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(leftType.has_value() && rightType.has_value() && binaryType.has_value());
    if (leftType && rightType && binaryType) {
        KAI_CHECK(*leftType == Type::i64());
        KAI_CHECK(*rightType == Type::i64());
        KAI_CHECK(*binaryType == Type::i64());
    }
}

void testFixedSiblingArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    let a = x + 1\n    let b = 1 + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& binaryA = static_cast<const BinaryExpr&>(declA.initializer());
    const auto& binaryB = static_cast<const BinaryExpr&>(declB.initializer());

    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i64());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::i64());
    }

    const auto literalInA = result.model.typeOf(binaryA.right());
    const auto literalInB = result.model.typeOf(binaryB.left());
    KAI_CHECK(literalInA.has_value() && literalInB.has_value());
    if (literalInA && literalInB) {
        KAI_CHECK(*literalInA == Type::i64());
        KAI_CHECK(*literalInB == Type::i64());
    }
}

void testNestedArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    let c = x + (1 + 2)\n    let d = (1 + 2) + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declD = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto cId = result.model.declarationSymbol(declC.name());
    const auto dId = result.model.declarationSymbol(declD.name());
    if (cId) {
        KAI_CHECK(result.model.symbol(*cId).type == Type::i64());
    }
    if (dId) {
        KAI_CHECK(result.model.symbol(*dId).type == Type::i64());
    }

    const auto& binaryC = static_cast<const BinaryExpr&>(declC.initializer());
    const auto& parenC = static_cast<const ParenExpr&>(binaryC.right());
    const auto& innerC = static_cast<const BinaryExpr&>(parenC.inner());

    const auto& binaryD = static_cast<const BinaryExpr&>(declD.initializer());
    const auto& parenD = static_cast<const ParenExpr&>(binaryD.left());
    const auto& innerD = static_cast<const BinaryExpr&>(parenD.inner());

    for (const BinaryExpr* inner : {&innerC, &innerD}) {
        const auto parenType = result.model.typeOf(*inner);
        const auto leftType = result.model.typeOf(inner->left());
        const auto rightType = result.model.typeOf(inner->right());
        KAI_CHECK(parenType.has_value() && leftType.has_value() && rightType.has_value());
        if (parenType && leftType && rightType) {
            KAI_CHECK(*parenType == Type::i64());
            KAI_CHECK(*leftType == Type::i64());
            KAI_CHECK(*rightType == Type::i64());
        }
    }
    const auto parenCType = result.model.typeOf(parenC);
    const auto parenDType = result.model.typeOf(parenD);
    KAI_CHECK(parenCType.has_value() && parenDType.has_value());
    if (parenCType && parenDType) {
        KAI_CHECK(*parenCType == Type::i64());
        KAI_CHECK(*parenDType == Type::i64());
    }
}

void testFloatSiblingArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: f32) {\n    let a = x + 1.5\n    let b = 1.5 + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::f32());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::f32());
    }
}

void testCrossFamilyArithmeticRejectsAdaptation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: f64) {\n    let y = x + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declY.initializer());

    const auto literalType = result.model.typeOf(binary.right());
    KAI_CHECK(literalType.has_value());
    if (literalType) {
        KAI_CHECK(*literalType == Type::i32());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 2: whole-result context must not contaminate comparison/equality ---

void testComparisonOperandsIgnoreWholeExpressionContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 < 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::i64());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(leftType.has_value() && rightType.has_value() && binaryType.has_value());
    if (leftType && rightType && binaryType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
        KAI_CHECK(*binaryType == Type::boolean());
    }
}

void testEqualityOperandsIgnoreWholeExpressionContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 == 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::i64());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    KAI_CHECK(leftType.has_value() && rightType.has_value());
    if (leftType && rightType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
    }
}

void testComparisonOverflowRegressionIgnoresOuterContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 2147483648 < 2147483649\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Outer i64 must NOT contextualize the comparison's operands - both
    // integer literals use the default I32 target and overflow it.
    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i32());
        }
        KAI_CHECK(!error.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(binaryType.has_value());
    if (binaryType) {
        // Left operand errored (LiteralOutOfRange) -> propagates to Error,
        // never a spurious InvalidBinaryOperands/TypeMismatch on top.
        KAI_CHECK(binaryType->isError());
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i64());
    }
}

void testComparisonEqualitySiblingAnchor() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n"
                                          "    let a = x < 1\n    let b = 1 < x\n"
                                          "    let c = x == 1\n    let d = 1 == x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }

    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);
    const auto& declD = static_cast<const VarDeclStmt&>(*fn.body().statements()[3]);
    const auto& binaryA = static_cast<const BinaryExpr&>(declA.initializer());
    const auto& binaryB = static_cast<const BinaryExpr&>(declB.initializer());
    const auto& binaryC = static_cast<const BinaryExpr&>(declC.initializer());
    const auto& binaryD = static_cast<const BinaryExpr&>(declD.initializer());

    const auto literalA = result.model.typeOf(binaryA.right());
    const auto literalB = result.model.typeOf(binaryB.left());
    const auto literalC = result.model.typeOf(binaryC.right());
    const auto literalD = result.model.typeOf(binaryD.left());
    for (const auto& literalType : {literalA, literalB, literalC, literalD}) {
        KAI_CHECK(literalType.has_value());
        if (literalType) {
            KAI_CHECK(*literalType == Type::i64());
        }
    }
}

// --- Milestone 2: annotation-span provenance through arithmetic ---

void testArithmeticAnnotationSpanPropagation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: u8 = 300 + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(sm.text(error.primarySpan) == "300");
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(sm.text(*error.relatedSpan) == "u8");
        }
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::u8());
    }
}

void testArithmeticSiblingAnchorDoesNotFabricateAnnotationSpan() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i8) {\n    let y = x + 200\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        // The anchor came from sibling `x`, not an explicit annotation -
        // no relatedSpan is fabricated for it.
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i8());
        }
    }
}

// --- Milestone 2: Range / Ref / RefMut remain fully deferred ---

void testRangeRemainsUnresolvedNoOperatorDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 1..2.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isUnresolved());
    }
}

void testRefAndRefMutRemainUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {\n    let a = &x\n    let b = &mut x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type.isUnresolved());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type.isUnresolved());
    }
}
} // namespace

int main() {
    testNegateSignedIntegerReturnsOperandType();
    testNegateFloatReturnsOperandType();
    testNegateUnsignedIntegerInvalidUnaryOperand();
    testNegateBoolInvalidUnaryOperand();
    testNotBoolReturnsBool();
    testNotIntegerInvalidUnaryOperand();
    testNegativeFloatLiteralRegression();

    testArithmeticSameTypeSucceeds();
    testArithmeticMixedTypesInvalidBinaryOperands();

    testModuloIntegerSucceeds();
    testModuloFloatInvalidBinaryOperands();

    testOrderingNumericSucceedsBool();
    testOrderingRejectsInvalidOperands();

    testLogicalBoolSucceeds();
    testLogicalNonBoolInvalidBinaryOperands();

    testEqualitySameTypeSucceeds();
    testEqualityRejectsCrossTypeAndUnit();

    testWholeExpressionArithmeticContext();
    testFixedSiblingArithmeticContextBothOrders();
    testNestedArithmeticContextBothOrders();
    testFloatSiblingArithmeticContextBothOrders();
    testCrossFamilyArithmeticRejectsAdaptation();

    testComparisonOperandsIgnoreWholeExpressionContext();
    testEqualityOperandsIgnoreWholeExpressionContext();
    testComparisonOverflowRegressionIgnoresOuterContext();
    testComparisonEqualitySiblingAnchor();

    testArithmeticAnnotationSpanPropagation();
    testArithmeticSiblingAnchorDoesNotFabricateAnnotationSpan();

    testRangeRemainsUnresolvedNoOperatorDiagnostic();
    testRefAndRefMutRemainUnresolved();

    return kai::test::failureCount == 0 ? 0 : 1;
}
