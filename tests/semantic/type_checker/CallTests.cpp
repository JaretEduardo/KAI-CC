#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- Milestone 3: direct user-function calls ---

void testDirectFunctionCallResultType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {\n    return 1\n}\nfn main() {\n    let a = f()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declA = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declA.initializer());
    const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

    const auto calleeType = result.model.typeOf(callee);
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(calleeType.has_value());
    KAI_CHECK(callType.has_value());
    if (calleeType && callType) {
        // Spec #4: the callee identifier itself never becomes a
        // first-class Function Type - only the CallExpr carries call
        // semantics.
        KAI_CHECK(calleeType->isUnresolved());
        KAI_CHECK(*callType == Type::i64());
    }

    const auto aId = result.model.declarationSymbol(declA.name());
    KAI_CHECK(aId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i64());
    }
}

void testUnitReturnCallResultType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn do_work() {}\nfn main() {\n    let result = do_work()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declResult = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declResult.initializer());

    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(*callType == Type::unit());
    }

    const auto resultId = result.model.declarationSymbol(declResult.name());
    KAI_CHECK(resultId.has_value());
    if (resultId) {
        KAI_CHECK(result.model.symbol(*resultId).type == Type::unit());
    }
}

// --- Milestone 3: argument contextual typing ---

void testArgumentContextualI64Literal() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: i64) {}\nfn main() {\n    take(10)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::i64());
    }
}

void testArgumentU8Fit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: u8) {}\nfn main() {\n    take(255)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testArgumentU8Overflow() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: u8) {}\nfn main() {\n    take(256)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }
}

void testArgumentF32Literal() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: f32) {}\nfn main() {\n    take(1.5)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::f32());
    }
}

void testArgumentNoIntToFloatAdaptation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: f64) {}\nfn main() {\n    take(1)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::f64());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::i32());
    }
}

void testArgumentAlreadyTypedI32Mismatch() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn take(x: i64) {}\nfn main() {\n    let value = 10\n    take(value)\n}");
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
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

// --- Milestone 3: contextual argument expressions ---

void testArgumentArithmeticContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: i64) {}\nfn main() {\n    take(1 + 2)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::i64());
    }
}

void testArgumentComparisonContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn take(x: bool) {}\nfn main() {\n    take(1 < 2)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::boolean());
    }
}

// --- Milestone 3: argument count ---

void testArgumentCountTooFew() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {}\nfn main() {\n    f()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidArgumentCount);
        KAI_CHECK(error.primarySpan == call.span());
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(!error.actualType.has_value());
    }

    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testArgumentCountTooMany() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {}\nfn main() {\n    f(1, 2)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidArgumentCount);
    }
}

void testExtraArgumentsStillVisited() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {}\nfn main() {\n    f(1, true)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    KAI_CHECK(call.arguments().size() == 2);

    const auto firstType = result.model.typeOf(*call.arguments()[0]);
    const auto secondType = result.model.typeOf(*call.arguments()[1]);
    KAI_CHECK(firstType.has_value());
    KAI_CHECK(secondType.has_value());
    if (firstType && secondType) {
        KAI_CHECK(*firstType == Type::i32());
        KAI_CHECK(*secondType == Type::boolean());
    }
}

// --- Milestone 3: multiple independent errors ---

void testTwoPositionalMismatches() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i32, b: bool) {}\nfn main() {\n    f(true, 1)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testMismatchPlusUnknownPlusWrongCount() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i64) {}\nfn main() {\n    f(true, unknown)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Deterministic order: SemanticAnalyzer's UnknownIdentifier (Pass 2
    // already walks every call argument) always precedes every
    // TypeChecker diagnostic; within TypeChecker, the shared-prefix
    // TypeMismatch precedes the count diagnostic, emitted last.
    KAI_CHECK(result.model.errors().size() == 3);
    if (result.model.errors().size() == 3) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::TypeMismatch);
        KAI_CHECK(result.model.errors()[2].kind == SemanticErrorKind::InvalidArgumentCount);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

// --- Milestone 3: recovery ---

void testArgumentErrorNoMismatchCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) -> i32 {}\nfn main() {\n    f(unknown)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testArgumentUnresolvedPreservesReturn() {
    // Parameter type is CONCRETE (i64) here, isolating the "argument
    // Unresolved" recovery case from the separate "parameter Unresolved"
    // case covered by testParameterUnresolvedPreservesReturnDespiteConcreteArgument.
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(x: i64) -> i64 {}\nfn main(value: &i32) {\n    let y = f(value)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declY = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declY.initializer());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(argType->isUnresolved());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::i64());
    }
}

void testParameterUnresolvedPreservesReturnDespiteConcreteArgument() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(x: &i32) -> i64 {}\nfn main() {\n    let value = 10\n    let y = f(value)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declY = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[1]);
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        // No TypeMismatch despite the argument being concretely I32 - the
        // parameter's own type is Unresolved, so no comparison is
        // meaningful, and the concrete return type is still preserved.
        KAI_CHECK(result.model.symbol(*yId).type == Type::i64());
    }
}

void testParameterErrorPreservesReturn() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: Foo) -> i64 {}\nfn main() {\n    let y = f(10)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declY = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declY.initializer());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        // Checked with no usable context (parameter is Error) - I32 default.
        KAI_CHECK(*argType == Type::i32());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::i64());
    }
}

void testReturnErrorProducesCallError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> Foo {}\nfn main() {\n    f()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType from the declaration - no new
    // diagnostic from the call site.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testReturnUnresolvedProducesCallUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> &i32 {}\nfn main() {\n    f()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isUnresolved());
    }
}

// --- Milestone 3: outer context never rewrites a call's return type ---

void testOuterExpectedDoesNotChangeReturnType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {}\nfn main() {\n    let x: i32 = f()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::i32());
            KAI_CHECK(*error.actualType == Type::i64());
        }
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declX = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declX.initializer());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(*callType == Type::i64());
    }
}

// --- Milestone 3: NotCallable ---

void testNotCallableLocalConcrete() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    let x = 1\n    x()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::NotCallable);
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& callStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    KAI_CHECK(result.model.errors()[0].primarySpan == call.callee().span());

    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testNotCallableParameterConcrete() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {\n    x()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::NotCallable);
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testNotCallableLiteralCallee() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    1()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::NotCallable);
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

// --- Milestone 3: no NotCallable ---

void testUnknownCalleeNoNotCallable() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    unknown(1)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& callStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        KAI_CHECK(*argType == Type::i32());
    }

    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isError());
    }
}

void testUnresolvedParameterCalleeNoNotCallable() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: &i32) {\n    x()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& callStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isUnresolved());
    }
}

void testDeferredCalleeShapesNoNotCallable() {
    // KAI LANGUAGE M7B: `arr[0]()` REMOVED from this list - indexing a
    // non-array `i32` is no longer a deferred/Unresolved shape, it is a
    // real SemanticErrorKind::InvalidIndexTarget error now (checkIndexExpr()
    // in TypeChecker.cpp), which would break this test's own
    // `errors().empty()` premise for an unrelated reason. Member (`.method()`)
    // and error-propagation (`?()`) callee shapes remain genuinely
    // deferred/Unresolved, unaffected by M7B, and still covered here.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32, result: i32) {\n"
                                          "    obj.method()\n"
                                          "    result?()\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& call = static_cast<const CallExpr&>(static_cast<const ExprStmt&>(*stmt).expr());
        const auto callType = result.model.typeOf(call);
        KAI_CHECK(callType.has_value());
        if (callType) {
            KAI_CHECK(callType->isUnresolved());
        }
    }
}

// --- Milestone 3: parenthesized direct function callees ---

void testParenthesizedFunctionCallee() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {}\nfn main() {\n    let b = (f)()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declB = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declB.initializer());
    const auto& paren = static_cast<const ParenExpr&>(call.callee());
    const auto& identifier = static_cast<const IdentifierExpr&>(paren.inner());

    const auto callType = result.model.typeOf(call);
    const auto parenType = result.model.typeOf(paren);
    const auto identifierType = result.model.typeOf(identifier);
    KAI_CHECK(callType.has_value() && parenType.has_value() && identifierType.has_value());
    if (callType && parenType && identifierType) {
        KAI_CHECK(*callType == Type::i64());
        KAI_CHECK(parenType->isUnresolved());
        KAI_CHECK(identifierType->isUnresolved());
    }
}

void testDeeplyParenthesizedFunctionCallee() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() -> i64 {}\nfn main() {\n    let c = (((f)))()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declC = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declC.initializer());

    const auto& outerParen = static_cast<const ParenExpr&>(call.callee());
    const auto& middleParen = static_cast<const ParenExpr&>(outerParen.inner());
    const auto& innerParen = static_cast<const ParenExpr&>(middleParen.inner());
    const auto& identifier = static_cast<const IdentifierExpr&>(innerParen.inner());

    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(*callType == Type::i64());
    }
    for (const ParenExpr* paren : {&outerParen, &middleParen, &innerParen}) {
        const auto parenType = result.model.typeOf(*paren);
        KAI_CHECK(parenType.has_value());
        if (parenType) {
            KAI_CHECK(parenType->isUnresolved());
        }
    }
    const auto identifierType = result.model.typeOf(identifier);
    KAI_CHECK(identifierType.has_value());
    if (identifierType) {
        KAI_CHECK(identifierType->isUnresolved());
    }
}

// --- Milestone 3: Builtin calls ---

void testBuiltinCallStaysUnresolvedUnchecked() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    print(1, 2, 3)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& callStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        KAI_CHECK(callType->isUnresolved());
    }
    for (const auto& argument : call.arguments()) {
        const auto argType = result.model.typeOf(*argument);
        KAI_CHECK(argType.has_value());
        if (argType) {
            KAI_CHECK(*argType == Type::i32());
        }
    }
}

void testBuiltinCallArgumentsStillTraversedWithChildError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    print(unknown)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& callStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(callType.has_value());
    if (callType) {
        // Deferred Builtin call semantics stay Unresolved even with an
        // Error child - unlike a validated user Function call.
        KAI_CHECK(callType->isUnresolved());
    }
}

void testUserPrintShadowsBuiltin() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn print(x: i64) {}\nfn main() {\n    print(10)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(argType.has_value());
    KAI_CHECK(callType.has_value());
    if (argType && callType) {
        // Contextualized to I64 and Unit-returning - proof this is the
        // real user Function, not deferred Builtin handling.
        KAI_CHECK(*argType == Type::i64());
        KAI_CHECK(*callType == Type::unit());
    }
}

// --- Milestone 3: integration ---

void testNestedCallsComposeWithoutSecondTraversal() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn id(x: i64) -> i64 {\n    return x\n}\n"
                                          "fn take(x: i64) {}\n"
                                          "fn main() {\n    take(id(10))\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[2]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& outerCall = static_cast<const CallExpr&>(callStmt.expr());
    const auto& innerCall = static_cast<const CallExpr&>(*outerCall.arguments()[0]);

    const auto literalType = result.model.typeOf(*innerCall.arguments()[0]);
    const auto innerCallType = result.model.typeOf(innerCall);
    const auto outerArgType = result.model.typeOf(*outerCall.arguments()[0]);
    const auto outerCallType = result.model.typeOf(outerCall);

    KAI_CHECK(literalType.has_value() && innerCallType.has_value() && outerArgType.has_value() &&
              outerCallType.has_value());
    if (literalType && innerCallType && outerArgType && outerCallType) {
        KAI_CHECK(*literalType == Type::i64());
        KAI_CHECK(*innerCallType == Type::i64());
        KAI_CHECK(*outerArgType == Type::i64());
        KAI_CHECK(*outerCallType == Type::unit());
    }
}

void testCallResultAnchorsArithmeticBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn get() -> i64 {}\nfn main() {\n"
                                          "    let a = get() + 1\n    let b = 1 + get()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declA = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i64());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::i64());
    }
}

void testFloatReturningCallRejectsIntegerLiteralAddition() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn get() -> f64 {}\nfn main() {\n    let x = get() + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& declX = static_cast<const VarDeclStmt&>(*mainFn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }
}

// --- Milestone 3: duplicate functions ---

void testDuplicateFunctionFirstWinsSignature() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn a(x: i32) {}\nfn a(x: i64) {}\nfn main() {\n    a(10)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::DuplicateSymbol);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[2]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());

    const auto argType = result.model.typeOf(*call.arguments()[0]);
    KAI_CHECK(argType.has_value());
    if (argType) {
        // The FIRST declaration's signature (i32) drives argument
        // contextual typing - the resolved callee SymbolId already
        // points at it, so the checker never re-derives this itself.
        KAI_CHECK(*argType == Type::i32());
    }
}
} // namespace

int main() {
    testDirectFunctionCallResultType();
    testUnitReturnCallResultType();

    testArgumentContextualI64Literal();
    testArgumentU8Fit();
    testArgumentU8Overflow();
    testArgumentF32Literal();
    testArgumentNoIntToFloatAdaptation();
    testArgumentAlreadyTypedI32Mismatch();

    testArgumentArithmeticContext();
    testArgumentComparisonContext();

    testArgumentCountTooFew();
    testArgumentCountTooMany();
    testExtraArgumentsStillVisited();

    testTwoPositionalMismatches();
    testMismatchPlusUnknownPlusWrongCount();

    testArgumentErrorNoMismatchCascade();
    testArgumentUnresolvedPreservesReturn();
    testParameterUnresolvedPreservesReturnDespiteConcreteArgument();
    testParameterErrorPreservesReturn();
    testReturnErrorProducesCallError();
    testReturnUnresolvedProducesCallUnresolved();

    testOuterExpectedDoesNotChangeReturnType();

    testNotCallableLocalConcrete();
    testNotCallableParameterConcrete();
    testNotCallableLiteralCallee();

    testUnknownCalleeNoNotCallable();
    testUnresolvedParameterCalleeNoNotCallable();
    testDeferredCalleeShapesNoNotCallable();

    testParenthesizedFunctionCallee();
    testDeeplyParenthesizedFunctionCallee();

    testBuiltinCallStaysUnresolvedUnchecked();
    testBuiltinCallArgumentsStillTraversedWithChildError();
    testUserPrintShadowsBuiltin();

    testNestedCallsComposeWithoutSecondTraversal();
    testCallResultAnchorsArithmeticBothOrders();
    testFloatReturningCallRejectsIntegerLiteralAddition();

    testDuplicateFunctionFirstWinsSignature();

    return kai::test::failureCount == 0 ? 0 : 1;
}
