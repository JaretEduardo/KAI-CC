#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- Milestone 5: conditions ---

void testIfTrueConditionValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if true {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testIfBoolIdentifierConditionValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(flag: bool) {\n    if flag {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testIfI32ConditionMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if 1 {\n    }\n}");
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
            KAI_CHECK(*error.expectedType == Type::boolean());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testIfArithmeticConditionMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if 1 + 2 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::boolean());
            // Bool context is not numeric - the arithmetic stays I32.
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testIfComparisonConditionValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if 1 < 2 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testIfBoolReturningCallValid() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn predicate() -> bool {\n    return true\n}\nfn f() {\n    if predicate() {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testIfI32ReturningCallMismatch() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn value() -> i32 {\n    return 1\n}\nfn f() {\n    if value() {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::boolean());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testIfAssignmentConditionMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut flag: bool = false\n    if flag = true {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::boolean());
            KAI_CHECK(*error.actualType == Type::unit());
        }
    }
}

void testIfErrorConditionNoCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if unknown {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

void testIfUnresolvedConditionNoMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: &i32) {\n    if x {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testElseIfConditionsIndependentlyChecked() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f() {\n    if 1 {\n    } else if true {\n    } else if 2 {\n    } else {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // The first and third conditions mismatch; the second is valid; the
    // final `else` has no condition of its own.
    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
    }
}

void testWhileBoolConditionValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    while true {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testWhileNonBoolConditionMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    while 1 {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }
}

void testInvalidConditionBodyStillTraversed() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    if 1 {\n        let x = unknown\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Deterministic order: SemanticAnalyzer's own UnknownIdentifier (Pass
    // 2 already walked the body's initializer) necessarily precedes
    // TypeChecker's condition TypeMismatch, since analyze() completes in
    // full before check() ever starts - independent of the fact that,
    // within TypeChecker's own pass, the condition is checked before the
    // body.
    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::TypeMismatch);
    }
}

// --- Milestone 5: returns ---

void testImplicitUnitBareReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    return\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testExplicitUnitBareReturnValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> () {\n    return\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testImplicitUnitReturnUnitValueValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    return ()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testExplicitUnitReturnUnitValueValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> () {\n    return ()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testUnitFunctionReturnUnitCallValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn do_work() {}\nfn f() {\n    return do_work()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testImplicitUnitReturnNonUnitMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    return 1\n}");
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
            KAI_CHECK(*error.expectedType == Type::unit());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testExplicitUnitReturnNonUnitMismatchRelatedSpan() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> () {\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(sm.text(*error.relatedSpan) == "()");
        }
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::unit());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testI64ReturnContextualLiteral() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& returnStmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    const auto literalType = result.model.typeOf(*returnStmt.value());
    KAI_CHECK(literalType.has_value());
    if (literalType) {
        KAI_CHECK(*literalType == Type::i64());
    }
}

void testU8ReturnFitValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> u8 {\n    return 255\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testU8ReturnOverflowAnnotationSpan() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> u8 {\n    return 300\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(sm.text(error.primarySpan) == "300");
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(sm.text(*error.relatedSpan) == "u8");
        }
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }
}

void testArithmeticReturnSubtreeContextualized() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1 + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& returnStmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    const auto binaryType = result.model.typeOf(*returnStmt.value());
    KAI_CHECK(binaryType.has_value());
    if (binaryType) {
        KAI_CHECK(*binaryType == Type::i64());
    }
}

void testArithmeticReturnOverflowAnnotationSpanPropagation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> u8 {\n    return 300 + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(sm.text(error.primarySpan) == "300");
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(sm.text(*error.relatedSpan) == "u8");
        }
    }
}

void testBoolVsI64ReturnMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return true\n}");
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
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }
}

void testErrorReturnExpressionNoCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

void testUnresolvedReturnExpressionNoMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: &i32) -> i64 {\n    return x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testDeclaredReturnTypeErrorNoExtraMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> Foo {\n    return\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType from the declaration - neither return
    // statement produces a new diagnostic.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }
}

void testDeclaredReturnTypeUnresolvedNoExtraMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> &i32 {\n    return\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testNonUnitBareReturnMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return\n}");
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
}

void testMultipleReturnsContinueAfterFirstMismatch() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(x: bool) -> i64 {\n    if x {\n        return true\n    }\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // The first (nested inside the if branch) mismatches; the second
    // (top-level, after the if) is a valid contextual I64 literal - both
    // are checked, proving ReturnContext threads correctly through If.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::i64());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }
}

void testNestedReturnInWhileUsesFunctionContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    while true {\n        return true\n    }\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i64());
        }
    }
}

void testNestedReturnInForUsesFunctionContext() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(items: i32) -> i64 {\n    for item in items {\n        return true\n    }\n    return 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i64());
        }
    }
}

void testEmptyNonUnitFunctionNoMissingReturnDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testConditionalOnlyReturnNoAllPathsDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(cond: bool) -> i64 {\n    if cond {\n        return 1\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testStatementAfterReturnStillChecked() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1\n    let x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }
}

// --- Milestone 5: example-shaped regressions ---

void testConditionsExampleShapedRegression() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn classify(value: i32) {\n"
                                          "    if value > 0 {\n    } else if value < 0 {\n    } else {\n    }\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testFunctionsExampleShapedRegression() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

// --- Full traversal ---

void testFullTraversalRecordsNestedExpressionTypes() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(value: i32) {\n"
                                          "    print(value, 1)\n"
                                          "    value + 2\n"
                                          "    [value, 3]\n"
                                          "    value[4]\n"
                                          "    value.field\n"
                                          "    value?\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& statements = fn.body().statements();
    KAI_CHECK(statements.size() == 6);
    if (statements.size() != 6) {
        return;
    }

    // print(value, 1) - Call.
    const auto& call = static_cast<const CallExpr&>(static_cast<const ExprStmt&>(*statements[0]).expr());
    KAI_CHECK(result.model.typeOf(*call.arguments()[0]).has_value());
    KAI_CHECK(result.model.typeOf(*call.arguments()[1]).has_value());

    // value + 2 - Binary.
    const auto& binary = static_cast<const BinaryExpr&>(static_cast<const ExprStmt&>(*statements[1]).expr());
    KAI_CHECK(result.model.typeOf(binary.left()).has_value());
    KAI_CHECK(result.model.typeOf(binary.right()).has_value());

    // [value, 3] - ArrayLiteral.
    const auto& array = static_cast<const ArrayLiteralExpr&>(static_cast<const ExprStmt&>(*statements[2]).expr());
    KAI_CHECK(array.elements().size() == 2);
    for (const auto& element : array.elements()) {
        KAI_CHECK(result.model.typeOf(*element).has_value());
    }

    // value[4] - Index.
    const auto& index = static_cast<const IndexExpr&>(static_cast<const ExprStmt&>(*statements[3]).expr());
    KAI_CHECK(result.model.typeOf(index.object()).has_value());
    KAI_CHECK(result.model.typeOf(index.index()).has_value());

    // value.field - Member.
    const auto& member = static_cast<const MemberExpr&>(static_cast<const ExprStmt&>(*statements[4]).expr());
    KAI_CHECK(result.model.typeOf(member.object()).has_value());

    // value? - ErrorPropagation.
    const auto& errorProp =
        static_cast<const ErrorPropagationExpr&>(static_cast<const ExprStmt&>(*statements[5]).expr());
    KAI_CHECK(result.model.typeOf(errorProp.operand()).has_value());
}
} // namespace

int main() {
    testIfTrueConditionValid();
    testIfBoolIdentifierConditionValid();
    testIfI32ConditionMismatch();
    testIfArithmeticConditionMismatch();
    testIfComparisonConditionValid();
    testIfBoolReturningCallValid();
    testIfI32ReturningCallMismatch();
    testIfAssignmentConditionMismatch();
    testIfErrorConditionNoCascade();
    testIfUnresolvedConditionNoMismatch();
    testElseIfConditionsIndependentlyChecked();
    testWhileBoolConditionValid();
    testWhileNonBoolConditionMismatch();
    testInvalidConditionBodyStillTraversed();

    testImplicitUnitBareReturnValid();
    testExplicitUnitBareReturnValid();
    testImplicitUnitReturnUnitValueValid();
    testExplicitUnitReturnUnitValueValid();
    testUnitFunctionReturnUnitCallValid();
    testImplicitUnitReturnNonUnitMismatch();
    testExplicitUnitReturnNonUnitMismatchRelatedSpan();
    testI64ReturnContextualLiteral();
    testU8ReturnFitValid();
    testU8ReturnOverflowAnnotationSpan();
    testArithmeticReturnSubtreeContextualized();
    testArithmeticReturnOverflowAnnotationSpanPropagation();
    testBoolVsI64ReturnMismatch();
    testErrorReturnExpressionNoCascade();
    testUnresolvedReturnExpressionNoMismatch();
    testDeclaredReturnTypeErrorNoExtraMismatch();
    testDeclaredReturnTypeUnresolvedNoExtraMismatch();
    testNonUnitBareReturnMismatch();
    testMultipleReturnsContinueAfterFirstMismatch();
    testNestedReturnInWhileUsesFunctionContext();
    testNestedReturnInForUsesFunctionContext();
    testEmptyNonUnitFunctionNoMissingReturnDiagnostic();
    testConditionalOnlyReturnNoAllPathsDiagnostic();
    testStatementAfterReturnStillChecked();

    testConditionsExampleShapedRegression();
    testFunctionsExampleShapedRegression();

    testFullTraversalRecordsNestedExpressionTypes();

    return kai::test::failureCount == 0 ? 0 : 1;
}
