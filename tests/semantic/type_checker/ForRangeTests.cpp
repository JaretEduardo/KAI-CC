// KAI LANGUAGE M6 (post-alpha.2): `for` + integer ranges - TypeChecker
// coverage for checkForStmt()/checkIntegerRangeFor() in TypeChecker.cpp.
// Kept in its own small file (mirroring this directory's per-milestone
// split, e.g. SpellableStrTests.cpp) rather than growing
// ConditionAndReturnTests.cpp, since this milestone's checks - range
// endpoint typing, loop-variable Symbol type/scope/immutability - are a
// distinct concern from that file's if/while/return condition coverage.
//
// What this file deliberately does NOT test (out of scope for M6, see
// the milestone contract): arrays/iterators/general iterable protocol,
// `..=`/reverse/step ranges, break/continue. Real native execution
// (stdout, start/end evaluated once, nested loops, return-inside-for,
// shadowing) is covered separately by NativeCompilationTests.cpp; LLVM
// IR structure (signed/unsigned comparison choice, CFG shape) by
// LLVMCodeGeneratorTests.cpp.

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

using kai::ast::ForStmt;

// A. A literal range - both endpoints default to i32 (no context, same
// rule as any other bare integer literal) - matches and is accepted.
void testLiteralRangeAcceptedAsI32() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    for i in 0..3 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::i32());
    }
}

// B. An endpoint variable: `for i in 0..n` with `n: u32` - the flexible
// literal `0` adapts to `n`'s concrete type via the SAME sibling-anchor
// mechanism arithmetic operators already use (checkMatchedOperands) - no
// new implicit-conversion system.
void testLiteralEndpointAdaptsToVariableEndpointType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(n: u32) {\n    for i in 0..n {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::u32());
    }
}

// C. Explicit signed endpoints (i64) - accepted, loop variable is i64.
void testExplicitSignedIntegerRangeAccepted() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let start: i64 = 0\n    let stop: i64 = 3\n    for i in start..stop {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[2]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::i64());
    }
}

// D. Explicit unsigned endpoints (u8) - accepted, loop variable is u8.
void testExplicitUnsignedIntegerRangeAccepted() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let start: u8 = 0\n    let stop: u8 = 3\n    for i in start..stop {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[2]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::u8());
    }
}

// E. Float endpoints are rejected via the existing InvalidBinaryOperands
// path (isIntegerDomain, not isNumericDomain) - the loop variable's
// Symbol type becomes Error.
void testFloatRangeRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    for i in 0.0..3.0 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::error());
    }
}

// F1. Bool endpoints are rejected the same way (non-integer domain).
void testBoolRangeRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    for i in true..false {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// F2. A non-Range iterable (a bare identifier) is its own distinct
// rejection - UnsupportedForIterable, never silently left Unresolved -
// since M6 has no general iterable protocol yet.
void testNonRangeIterableRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let values: i32 = 5\n    for i in values {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnsupportedForIterable);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);
    const auto id = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::error());
    }
}

// G. Scope: the loop variable is not visible after the loop ends -
// referencing it there is UnknownIdentifier, same as any other block-
// scoped local going out of scope.
void testLoopVariableNotVisibleAfterLoop() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    for i in 0..3 {\n    }\n    let x = i\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

// H. Shadowing: an outer `let i` and the loop's own `i` are distinct
// Symbols - the outer one's Symbol type is untouched by the loop's own
// range typing.
void testLoopVariableShadowsOuterBindingAsDistinctSymbol() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f() {\n    let i: i32 = 99\n    for i in 0..3 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);

    const auto outerId = result.model.declarationSymbol(outerDecl.name());
    const auto loopId = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(outerId.has_value());
    KAI_CHECK(loopId.has_value());
    if (outerId && loopId) {
        KAI_CHECK(*outerId != *loopId);
        KAI_CHECK(result.model.symbol(*outerId).type == Type::i32());
        KAI_CHECK(result.model.symbol(*loopId).type == Type::i32());
    }
}

// I. Assignment to the loop variable itself is rejected via the EXISTING
// AssignmentToImmutableBinding path - SemanticAnalyzer already declares
// it immutable (M1's analyzeForStmt()); M6 introduces no separate
// for-variable-specific diagnostic.
void testAssignmentToLoopVariableRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    for i in 0..3 {\n        i = i + 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// J. Mismatched concrete integer types (i32 vs u32, neither side a
// flexible literal) follow the SAME "no implicit conversion" rule
// arithmetic already enforces - InvalidBinaryOperands, not a silent
// widening/narrowing.
void testMismatchedConcreteIntegerTypesRejected() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a: i32 = 0\n    let b: u32 = 3\n    for i in a..b {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

} // namespace

int main() {
    testLiteralRangeAcceptedAsI32();
    testLiteralEndpointAdaptsToVariableEndpointType();
    testExplicitSignedIntegerRangeAccepted();
    testExplicitUnsignedIntegerRangeAccepted();
    testFloatRangeRejected();
    testBoolRangeRejected();
    testNonRangeIterableRejected();
    testLoopVariableNotVisibleAfterLoop();
    testLoopVariableShadowsOuterBindingAsDistinctSymbol();
    testAssignmentToLoopVariableRejected();
    testMismatchedConcreteIntegerTypesRejected();

    return kai::test::failureCount == 0 ? 0 : 1;
}
