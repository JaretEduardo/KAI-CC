// KAI LANGUAGE M7A: TypeChecker coverage for array LITERAL inference
// (checkArrayLiteralExpr() in TypeChecker.cpp) - homogeneous/incompatible
// elements, contextual (explicitly annotated) element typing, and the
// approved "[] is ambiguous without context" rule. Array TYPE syntax
// resolution (`[T; N]` itself, equality, interning, zero-length,
// nesting) is covered separately in ../ArrayTypeTests.cpp, since that
// needs SemanticAnalyzer alone, not TypeChecker.
//
// Deliberately does NOT cover: indexing (checkIndexExpr() is untouched
// in M7A - still unconditionally Type::unresolved(), full IndexExpr
// semantics belong to M7B), indexed assignment, or any LLVM lowering.

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

using kai::ast::ArrayLiteralExpr;
using kai::semantic::TypeKind;

// A. A homogeneous integer literal infers i32 per KAI's existing
// no-context integer-literal default - no new default is introduced.
void testHomogeneousIntegerLiteralInfersI32Array() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, 2, 3]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        const Type type = result.model.symbol(*id).type;
        KAI_CHECK(type.isArray());
        if (type.isArray()) {
            KAI_CHECK(result.model.arrayElementType(type) == Type::i32());
            KAI_CHECK(result.model.arrayLength(type) == 3);
        }
    }
}

// B. A homogeneous bool literal infers `[bool; N]` - proves the anchor-
// discovery path (Bool is never "flexible") works, not just the
// all-flexible integer-default path above.
void testHomogeneousBoolLiteralInfersBoolArray() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [true, false]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        const Type type = result.model.symbol(*id).type;
        KAI_CHECK(type.isArray());
        if (type.isArray()) {
            KAI_CHECK(result.model.arrayElementType(type) == Type::boolean());
            KAI_CHECK(result.model.arrayLength(type) == 2);
        }
    }
}

// C. An explicit contextual element type (`let xs: [u32; 3] = [0, 1, 2]`)
// adapts every flexible literal to it - the SAME sibling/contextual
// adaptation machinery arithmetic already uses, not a new system.
void testExplicitContextualElementTypeAdaptsLiterals() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs: [u32; 3] = [0, 1, 2]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        const Type type = result.model.symbol(*id).type;
        KAI_CHECK(type.isArray());
        if (type.isArray()) {
            KAI_CHECK(result.model.arrayElementType(type) == Type::u32());
        }
    }
}

// D. A length mismatch against an explicit annotation needs NO special
// array-specific diagnostic - it falls out of the ordinary
// initializerType == declaredType comparison, since [i32;3] != [i32;4]
// is now real structural inequality.
void testLengthMismatchAgainstAnnotationIsOrdinaryTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs: [i32; 3] = [1, 2, 3, 4]\n}");
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

// E. Incompatible elements (`[1, true, 3]`) are rejected -
// IncompatibleArrayElementType, not silently accepted or crashed on.
void testIncompatibleElementsRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = [1, true, 3]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::IncompatibleArrayElementType);
        KAI_CHECK(error.expectedType.has_value());
        KAI_CHECK(error.actualType.has_value());
        if (error.expectedType.has_value() && error.actualType.has_value()) {
            KAI_CHECK(*error.expectedType == Type::i32());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type.isError());
    }
}

// F. A standalone empty literal `[]` with no contextual array type is
// ambiguous - rejected, never given a fabricated/guessed element type.
void testStandaloneEmptyLiteralIsAmbiguous() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs = []\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AmbiguousEmptyArrayLiteral);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type.isError());
    }
}

// G. An explicitly contextually-typed empty literal (`let xs: [i32; 0] =
// []`) IS accepted (M7A spec §10/§5: zero-length arrays are approved).
void testContextuallyTypedEmptyLiteralAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs: [i32; 0] = []\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        const Type type = result.model.symbol(*id).type;
        KAI_CHECK(type.isArray());
        if (type.isArray()) {
            KAI_CHECK(result.model.arrayElementType(type) == Type::i32());
            KAI_CHECK(result.model.arrayLength(type) == 0);
        }
    }
}

// H. An empty literal contextually typed against a NON-zero declared
// length (`let xs: [i32; 3] = []`) still resolves the literal itself to
// length 0 - the resulting mismatch is an ordinary TypeMismatch, same
// as test D above, not a special "empty literal wrong length" error.
void testEmptyLiteralAgainstNonZeroLengthIsOrdinaryTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let xs: [i32; 3] = []\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.actualType.has_value()) {
            KAI_CHECK(result.model.arrayLength(*error.actualType) == 0);
        }
    }
}

// I. Each element is checked EXACTLY once - a call expression used as
// an anchor element must not have its own diagnostics duplicated. `f()`
// here is deliberately used as the array's FIRST (anchor-discovery)
// element with a genuinely wrong argument count, so a double-check bug
// would double this specific error.
void testEachElementCheckedExactlyOnceNoDuplicateDiagnostics() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) -> i32 {\n    return x\n}\n"
                                          "fn g() {\n    let xs = [f(), 1, 2]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // f() called with 0 arguments against a 1-parameter signature -
    // exactly ONE InvalidArgumentCount error, never two.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidArgumentCount);
    }
}

} // namespace

int main() {
    testHomogeneousIntegerLiteralInfersI32Array();
    testHomogeneousBoolLiteralInfersBoolArray();
    testExplicitContextualElementTypeAdaptsLiterals();
    testLengthMismatchAgainstAnnotationIsOrdinaryTypeMismatch();
    testIncompatibleElementsRejected();
    testStandaloneEmptyLiteralIsAmbiguous();
    testContextuallyTypedEmptyLiteralAccepted();
    testEmptyLiteralAgainstNonZeroLengthIsOrdinaryTypeMismatch();
    testEachElementCheckedExactlyOnceNoDuplicateDiagnostics();

    return kai::test::failureCount == 0 ? 0 : 1;
}
