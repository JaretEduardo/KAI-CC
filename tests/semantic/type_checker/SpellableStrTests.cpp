// Spellable str + Parameters/Returns MVP (M9): focused semantic-resolution
// and TypeChecker coverage for `str` becoming a spellable source-level type
// annotation (SemanticAnalyzer's lookupPrimitiveTypeName() now recognizes
// "str" -> Type::str() - see SemanticAnalyzer.cpp). Kept in its own small
// file (mirroring this directory's per-milestone split) rather than adding
// to the already-large SemanticAnalyzerTests.cpp/StringLiteralTests.cpp.
//
// Covers M9 spec §24 (semantic resolution) and §25 (TypeChecker) together,
// since both use the same analyzeAndCheck() pipeline and the facts being
// checked (a resolved Symbol/FunctionSignature type) come from the same
// SemanticAnalyzer + TypeChecker run.
//
// What this file deliberately does NOT test (out of scope for this
// milestone):
//   - String (still UnknownType - asserted below as a required regression)
//   - &str/&String/general reference semantics (Reference TypeSyntax still
//     resolves to Type::unresolved() unconditionally - asserted below)
//   - provenance/borrow checking beyond the one temporary
//     UnsupportedStrReturn rule (see TypeChecker.cpp's checkReturnStmt())
//   - operator domain regressions for Str (already covered by
//     StringLiteralTests.cpp's testStringConcatenationOperatorRemainsInvalid/
//     testStringEqualityRemainsInvalid - not duplicated here)

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- §24: semantic resolution ---

// A. `str` resolves as Type::str() through the ordinary named-type path,
// exactly like `i32`/`bool`/... - proven via a parameter annotation, which
// only SemanticAnalyzer's Pass 1 (FunctionSignature resolution) produces.
void testStrParameterResolvesAsStrType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn greet(name: str) {\n    print(name)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto fnId = result.model.declarationSymbol(fn.name());
    KAI_CHECK(fnId.has_value());
    if (fnId) {
        const auto& signature = result.model.symbol(*fnId).signature;
        KAI_CHECK(signature.has_value());
        if (signature) {
            KAI_CHECK(signature->parameterTypes.size() == 1);
            KAI_CHECK(signature->parameterTypes[0] == Type::str());
        }
    }
}

// B. explicit local annotation: `let message: str = "hello"` resolves the
// annotation to Type::str() and passes with no error - was UnknownType
// before this milestone (see StringLiteralTests.cpp's corrected test).
void testExplicitStrLocalAnnotationResolves() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let message: str = \"hello\"\n}");
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

// C. str parameter signature (duplicate-purpose check from a different
// angle than A: confirms the parameter's OWN declared-local Symbol, not
// just the FunctionSignature entry).
void testStrParameterSymbolHasStrType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn greet(name: str) {\n    print(name)\n}");
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());
    if (paramId) {
        KAI_CHECK(result.model.symbol(*paramId).type == Type::str());
    }
}

// D. str return signature.
void testStrReturnSignature() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn language() -> str {\n    return \"KAI\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto fnId = result.model.declarationSymbol(fn.name());
    KAI_CHECK(fnId.has_value());
    if (fnId) {
        const auto& signature = result.model.symbol(*fnId).signature;
        KAI_CHECK(signature.has_value());
        if (signature) {
            KAI_CHECK(signature->returnType == Type::str());
        }
    }
}

// E. REQUIRED regression (M9 spec §30): `String` remains UnknownType -
// SemanticAnalyzer's lookupPrimitiveTypeName() was NOT extended to
// recognize it.
void testStringAnnotationStillUnknownType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: String = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::UnknownType);
        KAI_CHECK(sm.text(error.primarySpan) == "String");
    }
}

// F. REQUIRED regression: `&str` remains unsupported - ReferenceTypeSyntax
// resolves to Type::unresolved() unconditionally (SemanticAnalyzer.cpp's
// resolveTypeSyntax() never inspects what a reference points at), so a
// `&str` parameter is silently deferred (Unresolved), never Type::str()
// and never a hard error - unchanged by this milestone, since reference
// semantics were deliberately not touched.
void testRefStrParameterStaysUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn greet(name: &str) {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto fnId = result.model.declarationSymbol(fn.name());
    KAI_CHECK(fnId.has_value());
    if (fnId) {
        const auto& signature = result.model.symbol(*fnId).signature;
        KAI_CHECK(signature.has_value());
        if (signature) {
            KAI_CHECK(signature->parameterTypes.size() == 1);
            KAI_CHECK(signature->parameterTypes[0].isUnresolved());
        }
    }
}

// --- §25: TypeChecker ---

// A. explicit str local accepts a literal initializer with no error.
void testExplicitStrLocalAcceptsLiteral() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let message: str = \"hello\"\n}");
    KAI_CHECK(result.model.errors().empty());
}

// B. explicit str local rejects an integer initializer (TypeMismatch).
void testExplicitStrLocalRejectsInteger() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: str = 42\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(error.expectedType.has_value() && *error.expectedType == Type::str());
    }
}

// C. str function argument accepts a string literal.
void testStrArgumentAcceptsString() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn greet(name: str) {\n    print(name)\n}\nfn main() {\n    greet(\"KAI\")\n}");
    KAI_CHECK(result.model.errors().empty());
}

// D. str function argument rejects an integer (TypeMismatch).
void testStrArgumentRejectsInteger() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn greet(name: str) {\n}\nfn main() {\n    greet(42)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(error.expectedType.has_value() && *error.expectedType == Type::str());
    }
}

// E. str return accepts a literal.
void testStrReturnAcceptsLiteral() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn language() -> str {\n    return \"KAI\"\n}");
    KAI_CHECK(result.model.errors().empty());
}

// F. str return accepts passthrough of the function's single str
// parameter (the elision-style safe case from the approved design).
void testStrReturnAcceptsSingleParameterPassthrough() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn identity(value: str) -> str {\n    return value\n}");
    KAI_CHECK(result.model.errors().empty());
}

// G. str return rejects an integer (TypeMismatch).
void testStrReturnRejectsInteger() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> str {\n    return 42\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(error.expectedType.has_value() && *error.expectedType == Type::str());
    }
}

// H1. Temporary multi-parameter return rule: rejects a non-literal return
// when the function has more than one str parameter (M9 spec §12's
// `choose` example) - UnsupportedStrReturn, not a fabricated TypeMismatch.
void testMultiStrParameterNonLiteralReturnRejected() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn choose(a: str, b: str) -> str {\n    return a\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnsupportedStrReturn);
    }
}

// H2. The same temporary rule does NOT reject a literal return even with
// more than one str parameter (M9 spec §12's `version` example) - a
// literal can never depend on any parameter's value.
void testMultiStrParameterLiteralReturnAllowed() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn version(a: str, b: str) -> str {\n    return \"KAI\"\n}");
    KAI_CHECK(result.model.errors().empty());
}

// J. mut str assignment works through the existing generic mutation
// rules - no string-specific assignment semantics were added.
void testMutStrAssignmentWorks() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut message: str = \"first\"\n    message = \"second\"\n}");
    KAI_CHECK(result.model.errors().empty());
}

} // namespace

int main() {
    testStrParameterResolvesAsStrType();
    testExplicitStrLocalAnnotationResolves();
    testStrParameterSymbolHasStrType();
    testStrReturnSignature();
    testStringAnnotationStillUnknownType();
    testRefStrParameterStaysUnresolved();

    testExplicitStrLocalAcceptsLiteral();
    testExplicitStrLocalRejectsInteger();
    testStrArgumentAcceptsString();
    testStrArgumentRejectsInteger();
    testStrReturnAcceptsLiteral();
    testStrReturnAcceptsSingleParameterPassthrough();
    testStrReturnRejectsInteger();
    testMultiStrParameterNonLiteralReturnRejected();
    testMultiStrParameterLiteralReturnAllowed();
    testMutStrAssignmentWorks();

    return kai::test::failureCount == 0 ? 0 : 1;
}
