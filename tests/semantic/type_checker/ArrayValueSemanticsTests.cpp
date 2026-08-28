// KAI LANGUAGE M8A: whole-array VALUE semantics + the function-boundary
// semantic contract (array parameters/returns are semantically by
// value). This is a DESIGN + SEMANTIC-CONTRACT milestone, not a backend
// one - every test in this file asserts FRONTEND (SemanticAnalyzer +
// TypeChecker) behavior only, via the SAME analyzeAndCheck() pipeline
// every other type_checker test file already uses. None of these tests
// require or assert LLVM codegen success: the M7B backend guards
// (declareFunction()'s explicit Array parameter/return rejection,
// lowerAssignmentExpr()'s/generateArrayVarDeclStmt()'s explicit whole-
// array-copy rejection) are INTENTIONALLY preserved until M8B - see
// LLVMCodeGeneratorTests.cpp/NativeCompilationTests.cpp for the
// dedicated "still cleanly fails at the backend" coverage instead.
//
// The audit finding this file locks in as tested, approved behavior:
// EVERY M8A semantic requirement (whole-array copy/assignment/self-
// assignment type-checks; array parameters/returns follow the exact
// same generic assignment-compatibility/argument-checking/return-type-
// checking machinery every other Type already uses) was ALREADY
// correct before this milestone, entirely because TypeChecker never
// special-cased "is this an aggregate" anywhere - Type equality/
// contextual-literal-adaptation/TypeMismatch are all Type-kind-agnostic.
// M8A therefore required ZERO TypeChecker.cpp/SemanticAnalyzer.cpp
// changes; this file is the "explicit and testable" half of that
// contract (M8A spec §17).

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- §18: whole-array VALUE semantics ---

// A. `let b = a` for two array-typed values is semantically valid - `b`
// receives its own value, structurally typed identically to `a` (no
// aliasing is modeled at the semantic layer at all: SemanticModel never
// tracks "these two Symbols share storage").
void testWholeArrayInitializationFromAnotherArrayIsValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let b = a\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto idA = result.model.declarationSymbol(declA.name());
    const auto idB = result.model.declarationSymbol(declB.name());
    KAI_CHECK(idA.has_value());
    KAI_CHECK(idB.has_value());
    if (idA && idB) {
        // Same structural TYPE (interning makes this a simple ==), but a
        // DIFFERENT SymbolId - two independent bindings, exactly what
        // "value copy, no aliasing" means at the semantic layer (there is
        // no third concept - like a shared storage/reference symbol -
        // that would represent aliasing here even if it existed).
        KAI_CHECK(result.model.symbol(*idA).type == result.model.symbol(*idB).type);
        KAI_CHECK(*idA != *idB);
    }
}

// B. `a = b` for two array-typed values, `a` mutable, exact same
// structural type - semantically valid (Unit-typed assignment, no
// error).
void testWholeArrayAssignmentToMutableBindingIsValid() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    mut a = [1, 2, 3]\n    let b = [4, 5, 6]\n    a = b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[2]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

// C. The SAME assignment through an IMMUTABLE binding is rejected via
// the EXISTING AssignmentToImmutableBinding path - no new, array-
// specific diagnostic.
void testWholeArrayAssignmentToImmutableBindingRejected() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let b = [4, 5, 6]\n    a = b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// D. A length mismatch (`[i32;3]` vs `[i32;4]`) is rejected via the
// EXISTING TypeMismatch path - real structural inequality does the
// work, no array-specific "length mismatch" diagnostic is invented.
void testWholeArrayAssignmentLengthMismatchRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    mut a: [i32; 3] = [1, 2, 3]\n"
                                          "    let b: [i32; 4] = [1, 2, 3, 4]\n"
                                          "    a = b\n"
                                          "}");
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
            KAI_CHECK(result.model.arrayLength(*error.expectedType) == 3);
            KAI_CHECK(result.model.arrayLength(*error.actualType) == 4);
        }
    }
}

// E. An element-type mismatch (`[i32;3]` vs `[u32;3]`) is rejected the
// same way - M8A spec §3: "No element-by-element implicit conversion
// between two already-typed array values."
void testWholeArrayAssignmentElementTypeMismatchRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    mut a: [i32; 3] = [1, 2, 3]\n"
                                          "    let b: [u32; 3] = [1, 2, 3]\n"
                                          "    a = b\n"
                                          "}");
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
            KAI_CHECK(result.model.arrayElementType(*error.expectedType) == Type::i32());
            KAI_CHECK(result.model.arrayElementType(*error.actualType) == Type::u32());
        }
    }
}

// F. Self-assignment (`a = a`) is semantically valid - no special
// language error is introduced for it (M8A spec §5).
void testWholeArraySelfAssignmentIsValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut a = [1, 2, 3]\n    a = a\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// --- §19: function PARAMETERS are semantically by value ---

// G. A call whose argument has the exact matching array type is
// accepted - reuses checkUserFunctionCall()'s existing generic
// contextual argument checking, no array-specific call-checking code.
void testFunctionCallWithExactArrayArgumentTypeAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn sum(xs: [i32; 3]) -> i32 {\n"
                                          "    return xs[0]\n"
                                          "}\n"
                                          "fn f() {\n"
                                          "    let a = [1, 2, 3]\n"
                                          "    sum(a)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// H. A wrong-length argument is rejected via the existing TypeMismatch
// path.
void testFunctionCallWithWrongLengthArgumentRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn sum(xs: [i32; 3]) -> i32 {\n"
                                          "    return xs[0]\n"
                                          "}\n"
                                          "fn f() {\n"
                                          "    let a = [1, 2, 3, 4]\n"
                                          "    sum(a)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

// I. A wrong ELEMENT type argument is rejected the same way.
void testFunctionCallWithWrongElementTypeArgumentRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn sum(xs: [i32; 3]) -> i32 {\n"
                                          "    return xs[0]\n"
                                          "}\n"
                                          "fn f() {\n"
                                          "    let a: [u32; 3] = [1, 2, 3]\n"
                                          "    sum(a)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

// J. An inline array LITERAL argument uses the SAME contextual-literal-
// adaptation machinery as everywhere else - `sum([1, 2, 3])` resolves
// the literal directly against the declared parameter type `[i32; 3]`,
// no new implicit-conversion system.
void testInlineArrayLiteralArgumentUsesContextualTyping() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn sum(xs: [i32; 3]) -> i32 {\n"
                                          "    return xs[0]\n"
                                          "}\n"
                                          "fn f() {\n"
                                          "    sum([1, 2, 3])\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// K. A zero-length array parameter type is semantically valid - no
// special exception either way (M8A spec §14).
void testZeroLengthArrayParameterAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(xs: [i32; 0]) -> i32 {\n"
                                          "    return 0\n"
                                          "}\n"
                                          "fn g() {\n"
                                          "    let a: [i32; 0] = []\n"
                                          "    f(a)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// L. Nested arrays as a parameter type are semantically valid too - the
// SAME generic argument-checking machinery, no special-casing for
// nesting (M8A spec §13: "M8A may document this... no multidimensional
// language redesign" - this falls out for free, nothing to redesign).
void testNestedArrayParameterAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(m: [[i32; 2]; 2]) -> i32 {\n"
                                          "    return m[0][0]\n"
                                          "}\n"
                                          "fn g() {\n"
                                          "    let m = [[1, 2], [3, 4]]\n"
                                          "    f(m)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// M. A `str` array parameter is semantically valid - M7B's existing
// `str` Copy/value contract (M8A spec §15) is preserved unchanged; this
// is NOT reinterpreted as owned-String semantics.
void testStrArrayParameterAccepted() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn first(names: [str; 2]) -> str {\n    return names[0]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// N. Array parameters follow KAI's EXISTING parameter-mutability rule
// exactly - no new mutable-parameter syntax is introduced (M8A spec
// §12): assigning to a WHOLE array parameter is still rejected exactly
// like assigning to any other parameter (parameters are always
// immutable - GRAMMAR.md §10 has no `mut` parameter syntax).
void testArrayParameterAssignmentRejectedLikeAnyParameter() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(xs: [i32; 3]) {\n    xs = [4, 5, 6]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// N2. Indexed assignment through an array PARAMETER also stays
// deferred (Unresolved, no diagnostic) - unaffected by M8A, exactly the
// pre-existing M7B "only a Local array base is a supported indexed-
// assignment target" rule (arrays-as-parameters were never in M7B's
// indexed-assignment scope, and M8A does not change that).
void testIndexedAssignmentThroughArrayParameterStaysDeferred() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(xs: [i32; 3]) {\n    xs[0] = 5\n}");
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

// --- §20: function RETURNS are semantically by value ---

// O. Returning a literal matching the declared array return type is
// valid - reuses checkReturnStmt()'s existing generic contextual
// checking, same as every other return type.
void testArrayReturnLiteralMatchingTypeAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn make() -> [i32; 3] {\n    return [1, 2, 3]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// P. A wrong-length return literal is rejected via the existing
// TypeMismatch path.
void testArrayReturnWrongLengthRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn make() -> [i32; 3] {\n    return [1, 2]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

// Q. A wrong ELEMENT type return value is rejected the same way.
void testArrayReturnWrongElementTypeRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn make() -> [i32; 3] {\n"
                                          "    let ys: [u32; 3] = [1, 2, 3]\n"
                                          "    return ys\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

// R. Returning an existing exact-type LOCAL array (not a fresh literal)
// is semantically valid - a by-value return of an already-bound array.
void testArrayReturnOfExistingExactTypeLocalAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn make() -> [i32; 3] {\n"
                                          "    let ys = [1, 2, 3]\n"
                                          "    return ys\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// S. An inline literal return contextually adapts to a non-default
// element type (`[u32; 3]`), exactly like §19's inline-argument case.
void testArrayReturnLiteralContextualElementTypeAdaptation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn make() -> [u32; 3] {\n    return [1, 2, 3]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

} // namespace

int main() {
    testWholeArrayInitializationFromAnotherArrayIsValid();
    testWholeArrayAssignmentToMutableBindingIsValid();
    testWholeArrayAssignmentToImmutableBindingRejected();
    testWholeArrayAssignmentLengthMismatchRejected();
    testWholeArrayAssignmentElementTypeMismatchRejected();
    testWholeArraySelfAssignmentIsValid();

    testFunctionCallWithExactArrayArgumentTypeAccepted();
    testFunctionCallWithWrongLengthArgumentRejected();
    testFunctionCallWithWrongElementTypeArgumentRejected();
    testInlineArrayLiteralArgumentUsesContextualTyping();
    testZeroLengthArrayParameterAccepted();
    testNestedArrayParameterAccepted();
    testStrArrayParameterAccepted();
    testArrayParameterAssignmentRejectedLikeAnyParameter();
    testIndexedAssignmentThroughArrayParameterStaysDeferred();

    testArrayReturnLiteralMatchingTypeAccepted();
    testArrayReturnWrongLengthRejected();
    testArrayReturnWrongElementTypeRejected();
    testArrayReturnOfExistingExactTypeLocalAccepted();
    testArrayReturnLiteralContextualElementTypeAdaptation();

    return kai::test::failureCount == 0 ? 0 : 1;
}
