#include "kai/semantic/ControlFlowAnalyzer.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::FunctionDecl;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticErrorKind;
using kai::semantic::SemanticModel;
using kai::semantic::Type;
using kai::semantic::TypeChecker;

namespace {

// Mirrors TypeCheckerTests.cpp's own Analyzed/Checked bundle, extended
// with the ControlFlowAnalyzer pass run on top of TypeChecker's output -
// the full pipeline: SourceManager -> Parser -> SemanticAnalyzer ->
// TypeChecker -> ControlFlowAnalyzer -> query the one, shared
// SemanticModel.
struct Checked {
    ParseResult<kai::ast::SourceFile> parsed;
    SemanticModel model;
};

Checked analyzeAndCheck(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();

    SemanticModel model;
    if (parsed.has_value()) {
        SemanticAnalyzer analyzer(sm);
        model = analyzer.analyze(*parsed);

        TypeChecker checker(sm);
        checker.check(*parsed, model);

        const ControlFlowAnalyzer flow;
        flow.check(*parsed, model);
    }

    return Checked{std::move(parsed), std::move(model)};
}

// --- Direct return completeness ---

void testNonUnitSingleReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testEmptyNonUnitFunctionMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

void testMissingReturnDiagnosticFields() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::MissingReturn);
        KAI_CHECK(sm.text(error.primarySpan) == "i64");
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i64());
        }
        KAI_CHECK(!error.actualType.has_value());
    }
}

void testImplicitUnitEmptyValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testExplicitUnitEmptyValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> () {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testUnitFunctionWithPartialExplicitReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(cond: bool) {\n    if cond {\n        return\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// --- if / else-if / else ---

void testIfWithoutElseMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(cond: bool) -> i64 {\n    if cond {\n        return 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

void testIfElseBothReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(cond: bool) -> i64 {\n    if cond {\n        return 1\n    } else {\n        return 2\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testElseIfChainAllReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) -> i64 {\n"
                                          "    if a {\n        return 1\n    } else if b {\n"
                                          "        return 2\n    } else {\n        return 3\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testElseIfChainOneBranchFallsThroughMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) -> i64 {\n"
                                          "    if a {\n        return 1\n    } else if b {\n"
                                          "    } else {\n        return 3\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

void testNestedIfAllPathsReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) -> i64 {\n"
                                          "    if a {\n"
                                          "        if b {\n            return 1\n        } else {\n"
                                          "            return 2\n        }\n"
                                          "    } else {\n        return 3\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testNestedIfOnePathFallsThroughMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) -> i64 {\n"
                                          "    if a {\n"
                                          "        if b {\n            return 1\n        }\n"
                                          "    } else {\n        return 2\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

// --- Return recovery / duplicate-diagnostic prevention ---

void testMismatchedReturnStillTerminatesPath() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

void testBothBranchesTerminateOneBadReturnNoMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm,
        "fn f(cond: bool) -> i64 {\n    if cond {\n        return true\n    } else {\n        return 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

void testBadBareReturnPlusMissingOtherPathBothFire() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(cond: bool) -> i64 {\n    if cond {\n        return\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // Two genuinely independent problems: the true-branch returns the
    // wrong type (bare return -> Unit, vs declared i64), and the false
    // path reaches the end with no return at all. Neither suppresses the
    // other.
    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::MissingReturn);
    }
}

// --- Error in condition does not affect structural flow ---

void testUnknownConditionReturningIfElseNoMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f() -> i64 {\n    if unknown {\n        return 1\n    } else {\n        return 2\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

// --- Loops: conservatively FallsThrough ---

void testWhileTrueReturnStillMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    while true {\n        return 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // Accepted CF-M1 limitation: a while loop is conservatively
    // FallsThrough regardless of its body or its (unexamined) condition.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

void testForReturnStillMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(items: i32) -> i64 {\n    for item in items {\n        return 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::MissingReturn);
    }
}

// --- Post-return: no unreachable-code analysis ---

void testStatementAfterReturnNoUnreachableDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1\n    let x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // Flow is complete (a bare `return 1` guarantees a return), so no
    // MissingReturn - but the later, structurally-unreachable statement's
    // own independent error must still be present (no unreachable-code
    // diagnostic exists, and no diagnostic traversal was skipped).
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

// --- Return type Error/Unresolved recovery ---

void testReturnTypeErrorNoMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> Foo {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // Exactly the one UnknownType from the declaration - no MissingReturn
    // cascade on top of an already-invalid return annotation.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }
}

void testReturnTypeUnresolvedNoMissingReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> &i32 {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// --- Realistic structure: guard clauses ---

void testGuardClauseFollowedByFallbackReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(cond: bool) -> i64 {\n    if cond {\n        return 1\n    }\n    return 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testMultipleGuardClausesFollowedByFallbackReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(op: i32) -> i64 {\n"
                                          "    if op == 1 {\n        return 10\n    }\n"
                                          "    if op == 2 {\n        return 20\n    }\n"
                                          "    if op == 3 {\n        return 30\n    }\n"
                                          "    return 0\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

} // namespace

int main() {
    testNonUnitSingleReturnValid();
    testEmptyNonUnitFunctionMissingReturn();
    testMissingReturnDiagnosticFields();
    testImplicitUnitEmptyValid();
    testExplicitUnitEmptyValid();
    testUnitFunctionWithPartialExplicitReturnValid();

    testIfWithoutElseMissingReturn();
    testIfElseBothReturnValid();
    testElseIfChainAllReturnValid();
    testElseIfChainOneBranchFallsThroughMissingReturn();
    testNestedIfAllPathsReturnValid();
    testNestedIfOnePathFallsThroughMissingReturn();

    testMismatchedReturnStillTerminatesPath();
    testBothBranchesTerminateOneBadReturnNoMissingReturn();
    testBadBareReturnPlusMissingOtherPathBothFire();

    testUnknownConditionReturningIfElseNoMissingReturn();

    testWhileTrueReturnStillMissingReturn();
    testForReturnStillMissingReturn();

    testStatementAfterReturnNoUnreachableDiagnostic();

    testReturnTypeErrorNoMissingReturn();
    testReturnTypeUnresolvedNoMissingReturn();

    testGuardClauseFollowedByFallbackReturnValid();
    testMultipleGuardClausesFollowedByFallbackReturnValid();

    return kai::test::failureCount == 0 ? 0 : 1;
}
