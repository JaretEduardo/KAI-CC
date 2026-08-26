// Minimal String Literal Support milestone: focused TypeChecker coverage
// for the new Type::str() (see Type.hpp's own comment) - kept in its own
// small file (mirroring this directory's per-milestone split, e.g.
// LiteralAndInferenceTests.cpp/OperatorTests.cpp/...) rather than growing
// one of the existing files, since this is a self-contained new topic.
//
// What this file deliberately does NOT test (out of scope for this
// milestone - see Type::str()'s own comment):
//   - String/&str/reference semantics/ownership/borrowing
//   - concatenation/formatting/string methods/indexing
//
// `str` as a spellable type annotation was out of scope for THIS milestone
// but has since been implemented (Spellable str + Parameters/Returns MVP) -
// see SpellableStrTests.cpp in this same directory for that coverage;
// testExplicitStrAnnotationStillUnknownType() below was updated in place
// rather than left asserting a claim that stopped being true.

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

// UPDATED (Spellable str + Parameters/Returns MVP): `str` is now a
// spellable source-level type annotation - SemanticAnalyzer's
// lookupPrimitiveTypeName() recognizes it (see SemanticAnalyzer.cpp). This
// test previously asserted the opposite (UnknownType); it is corrected
// here rather than left asserting a claim the compiler no longer makes.
// See SpellableStrTests.cpp for the fuller parameter/return/String/&str
// coverage this milestone added.
void testExplicitStrAnnotationNowResolves() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: str = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(declX.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(result.model.symbol(*id).type == Type::str());
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
    testExplicitStrAnnotationNowResolves();
    testStringConcatenationOperatorRemainsInvalid();
    testStringEqualityRemainsInvalid();

    return kai::test::failureCount == 0 ? 0 : 1;
}
