// KAI LANGUAGE M7B: TypeChecker coverage for real array IndexExpr typing
// (checkIndexExpr()/checkIndexAssignmentTarget()/tryDecodeConstantIndex()
// in TypeChecker.cpp) - index-domain validation, compile-time-constant
// bounds rejection, and indexed-assignment mutability/element-type rules.
// Array TYPE/LITERAL coverage lives separately in ../ArrayTypeTests.cpp
// and ArrayLiteralTests.cpp; native execution (reads/writes/dynamic
// bounds traps) lives in NativeCompilationTests.cpp; LLVM IR structure
// in LLVMCodeGeneratorTests.cpp.

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// A. IndexExpr result type is the array's own element type.
void testIndexResultTypeIsElementType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    let y = xs[0]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& yDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto id = result.model.declarationSymbol(yDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::i32());
    }
}

// B. Representative signed/unsigned index widths are all accepted.
void testSignedAndUnsignedIndexWidthsAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    let xs = [1, 2, 3]\n"
                                          "    let a: i8 = 0\n"
                                          "    let b: i64 = 1\n"
                                          "    let c: u8 = 0\n"
                                          "    let d: u64 = 2\n"
                                          "    print(xs[a])\n"
                                          "    print(xs[b])\n"
                                          "    print(xs[c])\n"
                                          "    print(xs[d])\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// C. A float index is rejected.
void testFloatIndexRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[0.0])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidIndexType);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType.has_value()) {
            KAI_CHECK(*error.actualType == Type::f64());
        }
    }
}

// D. A bool index is rejected.
void testBoolIndexRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[true])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidIndexType);
    }
}

// E. Indexing a non-array target is rejected - array indexing and `str`
// indexing remain separate, unrelated features (M7B spec §3/§9).
void testNonArrayTargetRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i32 = 5\n    print(x[0])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidIndexTarget);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType.has_value()) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

// F. Indexing a `str` is NOT accidentally routed into array indexing.
void testStrIndexingRejectedAsInvalidTarget() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let s = \"hello\"\n    print(s[0])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidIndexTarget);
    }
}

// G. A constant upper-bound out-of-bounds index is rejected at compile
// time - never an LLVM/backend error (M7B spec §22).
void testConstantUpperBoundOutOfBounds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[3])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::ArrayIndexOutOfBounds);
    }
}

// H. A constant negative index is rejected at compile time.
void testConstantNegativeOutOfBounds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[-1])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::ArrayIndexOutOfBounds);
    }
}

// H2. The exact boundary (length - 1) is valid, not rejected.
void testConstantExactUpperBoundaryValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[2])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// H3. A dynamic (non-literal) index is NOT bounds-checked at compile
// time at all - it stays a runtime concern (M7B spec §4: "do not build a
// general constant-folding engine solely for M7B").
void testDynamicIndexNotCompileTimeChecked() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    let i = 999\n    print(xs[i])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// I. Mutable indexed assignment is accepted.
void testMutableIndexedAssignmentAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut xs = [1, 2, 3]\n    xs[0] = 5\n}");
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

// J. Immutable indexed assignment is rejected via the EXISTING
// AssignmentToImmutableBinding path - no new, index-specific diagnostic.
void testImmutableIndexedAssignmentRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n    xs[0] = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// K. Wrong element assignment type is rejected via the existing
// TypeMismatch shape.
void testWrongElementAssignmentTypeRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut xs = [1, 2, 3]\n    xs[0] = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(error.expectedType.has_value());
        KAI_CHECK(error.actualType.has_value());
        if (error.expectedType.has_value() && error.actualType.has_value()) {
            KAI_CHECK(*error.expectedType == Type::i32());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }
}

// L. Contextual numeric adaptation for the assigned element - the SAME
// machinery checkVariableAssignmentTarget() already uses.
void testContextualNumericAdaptationForAssignedElement() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut xs: [u32; 3] = [0, 1, 2]\n    xs[0] = 9\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// M. The index expression is checked EXACTLY once - a call expression
// used as the index, with a genuinely wrong argument count, must not
// have its own diagnostic duplicated between the bounds-check attempt
// and the ordinary type-check.
void testIndexExpressionCheckedExactlyOnce() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn idx(x: i32) -> i32 {\n    return x\n}\n"
                                          "fn f() {\n    let xs = [1, 2, 3]\n    print(xs[idx()])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidArgumentCount);
    }
}

} // namespace

int main() {
    testIndexResultTypeIsElementType();
    testSignedAndUnsignedIndexWidthsAccepted();
    testFloatIndexRejected();
    testBoolIndexRejected();
    testNonArrayTargetRejected();
    testStrIndexingRejectedAsInvalidTarget();
    testConstantUpperBoundOutOfBounds();
    testConstantNegativeOutOfBounds();
    testConstantExactUpperBoundaryValid();
    testDynamicIndexNotCompileTimeChecked();
    testMutableIndexedAssignmentAccepted();
    testImmutableIndexedAssignmentRejected();
    testWrongElementAssignmentTypeRejected();
    testContextualNumericAdaptationForAssignedElement();
    testIndexExpressionCheckedExactlyOnce();

    return kai::test::failureCount == 0 ? 0 : 1;
}
