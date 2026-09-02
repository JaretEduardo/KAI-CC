#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- Milestone 4: valid mutable assignment ---

void testMutableAnnotatedReassignmentSuccess() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());

    const auto assignType = result.model.typeOf(assignment);
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(valueType.has_value());
    if (assignType && valueType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*valueType == Type::i64());
    }
}

void testMutableInferredReassignmentSuccess() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        // Assignment checking must never mutate a Symbol's recorded type.
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }

    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

// --- Milestone 4: immutability ---

void testImmutableLocalReassignmentError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::AssignmentToImmutableBinding);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(!error.actualType.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testImmutableParameterReassignmentError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

void testImmutabilityRelatedSpanPointsToDeclaration() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == declX.name().span);
        }
    }
}

void testImmutableTargetTypeMismatchDoesNotAlsoFire() {
    // examples/errors.kai's own regression shape, generalized: an
    // immutable target with an incompatible RHS type reports ONLY the
    // immutability error - never immutability + TypeMismatch together.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 0\n    x = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

void testImmutableTargetStillChecksRhsForIndependentErrors() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// --- Milestone 4: concrete target / RHS compatibility ---

void testMutableWrongConcreteTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(!error.relatedSpan.has_value());
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::i64());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testMutableI64ArithmeticContextualSubtree() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = 1 + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(valueType.has_value());
    if (valueType) {
        KAI_CHECK(*valueType == Type::i64());
    }
}

void testBoolTargetFromComparison() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut flag: bool = false\n    flag = 1 < 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(valueType.has_value());
    if (assignType && valueType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*valueType == Type::boolean());
    }
}

void testU8ContextualFit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: u8 = 0\n    x = 255\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testU8ContextualOverflow() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: u8 = 0\n    x = 256\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        // RHS became Error - no TypeMismatch is attempted on top.
        KAI_CHECK(assignType->isError());
    }
}

// --- Milestone 4: unknown / invalid targets ---

void testUnknownTargetNoExtraAssignmentDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testLiteralTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    1 = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Deterministic order: SemanticAnalyzer's UnknownIdentifier (RHS)
    // precedes TypeChecker's own InvalidAssignmentTarget.
    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::InvalidAssignmentTarget);
        KAI_CHECK(!result.model.errors()[1].expectedType.has_value());
        KAI_CHECK(!result.model.errors()[1].actualType.has_value());
        KAI_CHECK(!result.model.errors()[1].relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    KAI_CHECK(result.model.errors()[1].primarySpan == assignment.target().span());

    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testBinaryTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    1 + 2 = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& binaryTarget = static_cast<const BinaryExpr&>(assignment.target());

    const auto leftType = result.model.typeOf(binaryTarget.left());
    const auto rightType = result.model.typeOf(binaryTarget.right());
    KAI_CHECK(leftType.has_value());
    KAI_CHECK(rightType.has_value());
    if (leftType && rightType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
    }
}

void testCallTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn foo() -> i64 {\n    return 1\n}\nfn f() {\n    foo() = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto targetType = result.model.typeOf(assignment.target());
    KAI_CHECK(targetType.has_value());
    if (targetType) {
        // The call itself is still fully validated normally.
        KAI_CHECK(*targetType == Type::i64());
    }
}

void testFunctionIdentifierTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn foo() {}\nfn main() {\n    foo = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto targetType = result.model.typeOf(assignment.target());
    KAI_CHECK(targetType.has_value());
    if (targetType) {
        KAI_CHECK(targetType->isUnresolved());
    }
}

void testBuiltinIdentifierTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    print = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }
}

// --- Milestone 4: parenthesized targets ---

void testParenthesizedLocalTargetValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    (x) = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& paren = static_cast<const ParenExpr&>(assignment.target());

    const auto assignType = result.model.typeOf(assignment);
    const auto parenType = result.model.typeOf(paren);
    const auto identifierType = result.model.typeOf(paren.inner());
    KAI_CHECK(assignType.has_value() && parenType.has_value() && identifierType.has_value());
    if (assignType && parenType && identifierType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*parenType == Type::i64());
        KAI_CHECK(*identifierType == Type::i64());
    }
}

void testMultiplyParenthesizedLocalTargetValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    (((x))) = 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

// --- Milestone 4: target type Unresolved / Error recovery ---

void testTargetUnresolvedRhsNonErrorProducesUnit() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(value: &i32) {\n    mut y: &i32 = value\n    y = value\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

void testTargetUnresolvedRhsErrorProducesError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(value: &i32) {\n    mut y: &i32 = value\n    y = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testTargetErrorAlwaysError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: Foo = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType from the declaration - no new
    // diagnostic from the assignment itself.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());

    const auto valueType = result.model.typeOf(assignment.value());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(valueType.has_value());
    KAI_CHECK(assignType.has_value());
    if (valueType && assignType) {
        // RHS checked with no context (target type is Error) - I32 default.
        KAI_CHECK(*valueType == Type::i32());
        // Target Error is NOT treated like Unresolved - unconditionally Error.
        KAI_CHECK(assignType->isError());
    }
}

void testTargetErrorPreventsDownstreamBinaryOperandsCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: Foo = 0\n    let y = (x = 1) + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType - the Error assignment propagates into
    // the binary expression with NO InvalidBinaryOperands cascade.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& binary = static_cast<const BinaryExpr&>(declY.initializer());
    const auto& paren = static_cast<const ParenExpr&>(binary.left());
    const auto& assignment = static_cast<const AssignmentExpr&>(paren.inner());

    const auto assignType = result.model.typeOf(assignment);
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(binaryType.has_value());
    if (assignType && binaryType) {
        KAI_CHECK(assignType->isError());
        KAI_CHECK(binaryType->isError());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 4: Member / Index deferred targets ---

void testMemberTargetUnresolvedNoDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32) {\n    obj.field = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isUnresolved());
    }
}

// KAI LANGUAGE M7B: RETARGETED - this test's original premise ("indexed
// assignment into ANYTHING stays deferred with no diagnostic") is
// superseded outright: `xs[index] = value` for a mutable LOCAL array now
// really type-checks (see ArrayIndexAssignmentTests.cpp for that full
// coverage). KAI LANGUAGE M9 FINAL CLEANUP further supersedes this
// test's own M7B-era retargeting: indexing into an array-typed PARAMETER
// is no longer silently deferred either - a Parameter root is now a
// RECOGNIZED mutation target (same as a Local root), and since a
// parameter is always immutable (GRAMMAR.md §10), it is rejected via the
// EXISTING AssignmentToImmutableBinding diagnostic - the same diagnostic
// `let xs = ...; xs[0] = 1` already gets, no new parameter-specific one.
void testIndexIntoArrayParameterTargetRejectedAsImmutableBinding() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(arr: [i32; 3]) {\n    arr[0] = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::error());
    }
}

void testDeferredMemberTargetRhsErrorStillUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32) {\n    obj.field = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        // Deferred target FORM - a child Error does NOT propagate here,
        // unlike a valid mutable target (see testTargetUnresolvedRhsErrorProducesError).
        KAI_CHECK(assignType->isUnresolved());
        KAI_CHECK(!assignType->isError());
    }
}

// --- Milestone 4: expression-position / operator integration ---

void testLetYEqualsParenAssignInfersUnit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    let y = (x = 1)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& paren = static_cast<const ParenExpr&>(declY.initializer());
    const auto& assignment = static_cast<const AssignmentExpr&>(paren.inner());

    const auto assignType = result.model.typeOf(assignment);
    const auto parenType = result.model.typeOf(paren);
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(parenType.has_value());
    if (assignType && parenType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*parenType == Type::unit());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::unit());
    }
}

void testAssignmentPlusArithmeticInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    let y = (x = 1) + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

void testChainedAssignmentTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    mut x: i64 = 0\n    mut y: i64 = 0\n    mut z: i64 = 0\n"
                                          "    x = y = z\n}");
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
            KAI_CHECK(*error.actualType == Type::unit());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[3]);
    const auto& outer = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& inner = static_cast<const AssignmentExpr&>(outer.value());

    const auto innerType = result.model.typeOf(inner);
    const auto outerType = result.model.typeOf(outer);
    KAI_CHECK(innerType.has_value());
    KAI_CHECK(outerType.has_value());
    if (innerType && outerType) {
        KAI_CHECK(*innerType == Type::unit());
        KAI_CHECK(outerType->isError());
    }
}

void testRhsErrorProducesAssignmentErrorNoTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}
} // namespace

int main() {
    testMutableAnnotatedReassignmentSuccess();
    testMutableInferredReassignmentSuccess();

    testImmutableLocalReassignmentError();
    testImmutableParameterReassignmentError();
    testImmutabilityRelatedSpanPointsToDeclaration();
    testImmutableTargetTypeMismatchDoesNotAlsoFire();
    testImmutableTargetStillChecksRhsForIndependentErrors();

    testMutableWrongConcreteTypeMismatch();
    testMutableI64ArithmeticContextualSubtree();
    testBoolTargetFromComparison();
    testU8ContextualFit();
    testU8ContextualOverflow();

    testUnknownTargetNoExtraAssignmentDiagnostic();
    testLiteralTargetInvalid();
    testBinaryTargetInvalid();
    testCallTargetInvalid();
    testFunctionIdentifierTargetInvalid();
    testBuiltinIdentifierTargetInvalid();

    testParenthesizedLocalTargetValid();
    testMultiplyParenthesizedLocalTargetValid();

    testTargetUnresolvedRhsNonErrorProducesUnit();
    testTargetUnresolvedRhsErrorProducesError();
    testTargetErrorAlwaysError();
    testTargetErrorPreventsDownstreamBinaryOperandsCascade();

    testMemberTargetUnresolvedNoDiagnostic();
    testIndexIntoArrayParameterTargetRejectedAsImmutableBinding();
    testDeferredMemberTargetRhsErrorStillUnresolved();

    testLetYEqualsParenAssignInfersUnit();
    testAssignmentPlusArithmeticInvalidBinaryOperands();
    testChainedAssignmentTypeMismatch();
    testRhsErrorProducesAssignmentErrorNoTypeMismatch();

    return kai::test::failureCount == 0 ? 0 : 1;
}
