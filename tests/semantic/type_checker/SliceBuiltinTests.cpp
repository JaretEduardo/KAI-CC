// KAI LANGUAGE M10B: TypeChecker coverage for the `slice(...)`/`len(...)`
// builtins (checkSliceBuiltinCall()/checkLenBuiltinCall() in
// TypeChecker.cpp) and for Slice-aware indexing (checkIndexExpr()'s own
// Slice branch) and indexed-assignment rejection
// (checkIndexAssignmentTarget()'s own Slice branch). Slice TYPE
// resolution itself (SemanticAnalyzer-only) is covered separately in
// ../SliceTypeTests.cpp; native execution (reads/writes/dynamic bounds
// traps/parameter passing) lives in NativeCompilationTests.cpp; LLVM IR
// structure in LLVMCodeGeneratorTests.cpp.

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// A. `slice(a)` for a local fixed array resolves to `[T]`.
void testSliceOfLocalArrayResolvesToSliceType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& sDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto id = result.model.declarationSymbol(sDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        const Type type = result.model.symbol(*id).type;
        KAI_CHECK(type.isSlice());
        if (type.isSlice()) {
            KAI_CHECK(result.model.sliceElementType(type) == Type::i32());
        }
    }
}

// B. `slice(xs)` for a fixed-array PARAMETER resolves to `[T]` too.
void testSliceOfArrayParameterResolvesToSliceType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(xs: [i32; 3]) {\n    let s = slice(xs)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& sDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(sDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type.isSlice());
    }
}

// C. `slice(123)` - a non-array argument - is InvalidSliceSource.
void testSliceOfNonArrayRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let s = slice(123)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidSliceSource);
        KAI_CHECK(result.model.errors()[0].actualType.has_value());
        if (result.model.errors()[0].actualType.has_value()) {
            KAI_CHECK(*result.model.errors()[0].actualType == Type::i32());
        }
    }
}

// D. `slice(existingSlice)` - an existing Slice, not an Array - is
// InvalidSliceSource. Slice-of-Slice is never allowed to arise this way.
void testSliceOfExistingSliceRejected() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let t = slice(s)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidSliceSource);
    }
}

// E. `slice([1, 2, 3])` - an array LITERAL, not a direct binding - is
// InvalidSliceSource, even though the literal's own Type is a perfectly
// real Array (the rejection is about SHAPE/provenance, not the Type).
void testSliceOfArrayLiteralRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let s = slice([1, 2, 3])\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidSliceSource);
    }
}

// reject slice(str) - a `str` is not an array, and str is never
// reinterpreted as a byte slice.
void testSliceOfStrRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let s = slice(\"hello\")\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidSliceSource);
    }
}

// `slice(...)` with the wrong argument count is InvalidArgumentCount.
void testSliceWrongArgumentCountRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a, a)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidArgumentCount);
    }
}

// F. No implicit Array -> Slice conversion at a call site: passing a
// fixed array directly where a Slice parameter is expected remains a
// genuine TypeMismatch (spec §2/§20 - the caller must write
// `sum(slice(a))` explicitly).
void testNoImplicitArrayToSliceCallConversion() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn sum(xs: [i32]) -> i32 {\n    return 0\n}\nfn f() {\n    let a = [1, 2, 3]\n    sum(a)\n}");
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
            KAI_CHECK(error.expectedType->isSlice());
            KAI_CHECK(error.actualType->isArray());
        }
    }
}

// Q. The explicit, correct form - `sum(slice(a))` - type-checks cleanly.
void testExplicitSliceCallArgumentAccepted() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn sum(xs: [i32]) -> i32 {\n    return 0\n}\nfn f() {\n    let a = [1, 2, 3]\n    sum(slice(a))\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// G. `len(array)` -> u64.
void testLenOfArrayIsU64() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let n = len(a)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& nDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto id = result.model.declarationSymbol(nDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::u64());
    }
}

// H. `len(slice)` -> u64.
void testLenOfSliceIsU64() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let n = len(s)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& nDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);
    const auto id = result.model.declarationSymbol(nDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::u64());
    }
}

// I. `len(str)` -> u64.
void testLenOfStrIsU64() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let n = len(\"abc\")\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& nDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(nDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::u64());
    }
}

// J. `len(x)` for an unsupported domain (bool) is InvalidLenOperand.
void testLenOfUnsupportedOperandRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let n = len(true)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidLenOperand);
        KAI_CHECK(result.model.errors()[0].actualType.has_value());
        if (result.model.errors()[0].actualType.has_value()) {
            KAI_CHECK(*result.model.errors()[0].actualType == Type::boolean());
        }
    }
}

void testLenWrongArgumentCountRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let n = len()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidArgumentCount);
    }
}

// K. Slice IndexExpr result type is the SLICE's element type.
void testSliceIndexResultTypeIsElementType() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let y = s[0]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& yDecl = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);
    const auto id = result.model.declarationSymbol(yDecl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::i32());
    }
}

// L. Signed and unsigned index widths are both accepted for Slice
// indexing, exactly like array indexing.
void testSliceIndexAcceptsSignedAndUnsignedWidths() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    let a = [1, 2, 3]\n"
                                          "    let s = slice(a)\n"
                                          "    let i: i8 = 0\n"
                                          "    let u: u64 = 0\n"
                                          "    let x = s[i]\n"
                                          "    let y = s[u]\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// M. A float index into a Slice is InvalidIndexType - the SAME diagnostic
// array indexing already uses (no new "slice index type" diagnostic).
void testSliceIndexRejectsFloatIndex() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let y = s[1.0]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidIndexType);
    }
}

// N. A directly-known negative constant Slice index is rejected via
// SliceIndexOutOfBounds - a DIFFERENT kind from ArrayIndexOutOfBounds
// (spec §17: reused wording would be misleading for a runtime-length
// type).
void testSliceIndexRejectsConstantNegativeIndex() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let y = s[-1]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::SliceIndexOutOfBounds);
    }
}

// A large positive constant Slice index is NOT rejected at compile time -
// a slice's length is runtime data, so this can only be checked
// dynamically (spec §17).
void testSliceIndexPositiveConstantNotCompileTimeRejected() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    let y = s[999]\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// O. `s[index] = value` for a Slice `s` is ALWAYS rejected via
// AssignmentThroughImmutableSlice - a DIFFERENT diagnostic from
// AssignmentToImmutableBinding (spec §14).
void testSliceIndexedWriteRejectedAsImmutableSlice() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    s[0] = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentThroughImmutableSlice);
    }
}

// P. A MUTABLE Slice binding still cannot mutate elements - binding
// mutability and view-element mutability are distinct (spec §13/§14).
void testMutableSliceBindingStillRejectsIndexedWrite() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    let a = [1, 2, 3]\n    mut s = slice(a)\n    s[0] = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        // NOT AssignmentToImmutableBinding - `s` itself is `mut`.
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentThroughImmutableSlice);
    }
}

// A mutable Slice binding CAN be reassigned as a whole view (ordinary
// value reassignment, unrelated to element mutability - spec §13).
void testMutableSliceBindingCanBeReassignedAsWholeView() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    let a = [1, 2, 3]\n"
                                          "    let b = [4, 5, 6]\n"
                                          "    mut s = slice(a)\n"
                                          "    s = slice(b)\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// R. A Slice RETURN stays type-correct even though it remains backend/
// lifetime-deferred (spec §5/§24) - type correctness and safe executable
// behavior are different questions; this file only asserts the former.
void testSliceReturnTypeChecksCleanly() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn identity(xs: [i32]) -> [i32] {\n    return xs\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

} // namespace

int main() {
    testSliceOfLocalArrayResolvesToSliceType();
    testSliceOfArrayParameterResolvesToSliceType();
    testSliceOfNonArrayRejected();
    testSliceOfExistingSliceRejected();
    testSliceOfArrayLiteralRejected();
    testSliceOfStrRejected();
    testSliceWrongArgumentCountRejected();
    testNoImplicitArrayToSliceCallConversion();
    testExplicitSliceCallArgumentAccepted();
    testLenOfArrayIsU64();
    testLenOfSliceIsU64();
    testLenOfStrIsU64();
    testLenOfUnsupportedOperandRejected();
    testLenWrongArgumentCountRejected();
    testSliceIndexResultTypeIsElementType();
    testSliceIndexAcceptsSignedAndUnsignedWidths();
    testSliceIndexRejectsFloatIndex();
    testSliceIndexRejectsConstantNegativeIndex();
    testSliceIndexPositiveConstantNotCompileTimeRejected();
    testSliceIndexedWriteRejectedAsImmutableSlice();
    testMutableSliceBindingStillRejectsIndexedWrite();
    testMutableSliceBindingCanBeReassignedAsWholeView();
    testSliceReturnTypeChecksCleanly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
