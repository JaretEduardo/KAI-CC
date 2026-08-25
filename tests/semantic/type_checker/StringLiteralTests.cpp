// Minimal String Literal Support milestone: focused TypeChecker coverage
// for the new Type::str() (see Type.hpp's own comment) - kept in its own
// small file (mirroring this directory's per-milestone split, e.g.
// LiteralAndInferenceTests.cpp/OperatorTests.cpp/...) rather than growing
// one of the existing files, since this is a self-contained new topic.
//
// What this file deliberately does NOT test (out of scope for this
// milestone - see Type::str()'s own comment):
//   - `str` as a spellable type annotation (still UnknownType - asserted
//     below as a required regression, not a feature)
//   - String/&str/reference semantics/ownership/borrowing
//   - concatenation/formatting/string methods/indexing

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// A string literal's own Type is the concrete Type::str(), not
// Type::unresolved() - see checkLiteralExpr()'s LiteralKind::String case.
void testStringLiteralHasStrType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto stringType = result.model.typeOf(declX.initializer());
    KAI_CHECK(stringType.has_value());
    if (stringType) {
        KAI_CHECK(*stringType == Type::str());
    }
}

// Existing local-inference machinery (checkVarDecl()'s unannotated path)
// naturally infers Str for `let message = "..."` - no string-specific
// inference logic was added.
void testInferredLocalFromStringLiteralHasStrType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let message = \"Hello, KAI!\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declMessage = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(declMessage.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::str());
    }
}

// REQUIRED regression (M8 spec #18): `str` remains intentionally
// unspellable as a source type annotation. SemanticAnalyzer's
// lookupPrimitiveTypeName() was NOT changed to recognize it - this must
// still resolve exactly like any other unknown named type (Result,
// Buffer, ...): UnknownType, Type::error(), never Type::str().
void testExplicitStrAnnotationStillUnknownType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: str = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::UnknownType);
        KAI_CHECK(sm.text(error.primarySpan) == "str");
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(declX.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::error());
    }
}

// REQUIRED regression (M8 spec #19): Str does not accidentally gain
// operator support - `"a" + "b"` is rejected by the SAME general
// isNumericDomain-based InvalidBinaryOperands check every other non-
// numeric operand type already goes through; no Str-specific rejection
// code exists or was added.
void testStringConcatenationOperatorRemainsInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = \"a\" + \"b\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// REQUIRED regression (M8 spec #19): equality is likewise still rejected
// for Str - isEqualityDomain() (isNumeric() || isBool() || isChar())
// excludes it exactly as it did before this milestone.
void testStringEqualityRemainsInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = \"a\" == \"b\"\n}");
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
    testStringLiteralHasStrType();
    testInferredLocalFromStringLiteralHasStrType();
    testExplicitStrAnnotationStillUnknownType();
    testStringConcatenationOperatorRemainsInvalid();
    testStringEqualityRemainsInvalid();

    return kai::test::failureCount == 0 ? 0 : 1;
}
