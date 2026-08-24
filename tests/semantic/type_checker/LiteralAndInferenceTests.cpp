#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

// --- SemanticModel expression map: nullopt before, has-value after ---

void testExpressionMapIsNulloptBeforeTypeCheckerAndHasValueAfter() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn f() {\n    let x = 1\n}");
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();

    KAI_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    SemanticAnalyzer analyzer(sm);
    SemanticModel model = analyzer.analyze(*parsed);

    const auto& fn = static_cast<const FunctionDecl&>(*parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    // Before TypeChecker has run at all: no entry.
    KAI_CHECK(!model.typeOf(declX.initializer()).has_value());

    TypeChecker checker(sm);
    checker.check(*parsed, model);

    const auto afterType = model.typeOf(declX.initializer());
    KAI_CHECK(afterType.has_value());
    if (afterType) {
        KAI_CHECK(*afterType == Type::i32());
    }
}

void testDeferredExpressionStillRecordsUnresolvedEntry() {
    // `+` became an implemented operator in Milestone 2, so this M1
    // regression now uses Range - still fully deferred - as its
    // "unsupported/deferred expression" example instead.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0..1\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(binaryType.has_value());
    if (binaryType) {
        KAI_CHECK(binaryType->isUnresolved());
    }
}

// --- Defaults ---

void testDefaultIntegerLiteralInfersI32() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 10\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }

    const auto literalType = result.model.typeOf(declX.initializer());
    KAI_CHECK(literalType.has_value());
    if (literalType) {
        KAI_CHECK(*literalType == Type::i32());
    }
}

void testDefaultFloatLiteralInfersF64() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 1.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::f64());
    }
}

void testDefaultBoolLiteralInfersBool() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::boolean());
    }
}

void testDefaultCharLiteralInfersChar() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 'a'\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::character());
    }
}

void testDefaultUnitLiteralInfersUnit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = ()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::unit());
    }
}

// --- Integer contexts ---

void testIntegerContextualTypingAdaptsToDeclaredType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a: i64 = 10\n    let b: u8 = 255\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto aType = result.model.typeOf(declA.initializer());
    KAI_CHECK(aType.has_value());
    if (aType) {
        KAI_CHECK(*aType == Type::i64());
    }

    const auto bType = result.model.typeOf(declB.initializer());
    KAI_CHECK(bType.has_value());
    if (bType) {
        KAI_CHECK(*bType == Type::u8());
    }
}

// --- Integer overflow ---

void testIntegerOverflowProducesLiteralOutOfRange() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let a: u8 = 256\n    let b = 2147483648\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        const auto& first = result.model.errors()[0];
        KAI_CHECK(first.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(first.expectedType.has_value());
        if (first.expectedType) {
            KAI_CHECK(*first.expectedType == Type::u8());
        }
        KAI_CHECK(first.relatedSpan.has_value());

        const auto& second = result.model.errors()[1];
        KAI_CHECK(second.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(second.expectedType.has_value());
        if (second.expectedType) {
            KAI_CHECK(*second.expectedType == Type::i32());
        }
        // No explicit annotation on `b` - unconstrained I32 overflow.
        KAI_CHECK(!second.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto aType = result.model.typeOf(declA.initializer());
    KAI_CHECK(aType.has_value());
    if (aType) {
        KAI_CHECK(aType->isError());
    }
}

// --- Signed boundary ---

void testSignedBoundaryLiterals() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    let a: i8 = -128\n"
                                          "    let b: i8 = -129\n"
                                          "    let c = -2147483648\n"
                                          "    let d = -2147483649\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the two out-of-range boundary cases (b, d) produce errors.
    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);
    const auto& declD = static_cast<const VarDeclStmt&>(*fn.body().statements()[3]);

    const auto aType = result.model.typeOf(declA.initializer());
    KAI_CHECK(aType.has_value());
    if (aType) {
        KAI_CHECK(*aType == Type::i8());
    }

    const auto bType = result.model.typeOf(declB.initializer());
    KAI_CHECK(bType.has_value());
    if (bType) {
        KAI_CHECK(bType->isError());
    }

    const auto cType = result.model.typeOf(declC.initializer());
    KAI_CHECK(cType.has_value());
    if (cType) {
        KAI_CHECK(*cType == Type::i32());
    }

    const auto dType = result.model.typeOf(declD.initializer());
    KAI_CHECK(dType.has_value());
    if (dType) {
        KAI_CHECK(dType->isError());
    }
}

// --- Unsigned negative literal ---

void testUnsignedNegativeLiteralOutOfRange() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: u8 = -1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }
}

// --- Float context ---

void testFloatContextualTyping() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: f32 = 1.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xType = result.model.typeOf(declX.initializer());
    KAI_CHECK(xType.has_value());
    if (xType) {
        KAI_CHECK(*xType == Type::f32());
    }
}

// --- No int -> float contextual typing ---

void testIntegerLiteralDoesNotAdaptToFloatContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: f64 = 1\n}");
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
        if (error.expectedType && error.actualType) {
            KAI_CHECK(*error.expectedType == Type::f64());
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xType = result.model.typeOf(declX.initializer());
    KAI_CHECK(xType.has_value());
    if (xType) {
        KAI_CHECK(*xType == Type::i32());
    }
}

// --- No float -> int contextual typing ---

void testFloatLiteralDoesNotAdaptToIntegerContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i32 = 1.5\n}");
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
            KAI_CHECK(*error.actualType == Type::f64());
        }
    }
}

// --- Fixed literal mismatch ---

void testFixedLiteralTypesNeverAdapt() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    let a: i64 = true\n"
                                          "    let b: bool = 'a'\n"
                                          "    let c: i32 = ()\n"
                                          "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 3);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::TypeMismatch);
    }
}

// --- Parenthesized contextual typing ---

void testParenthesizedContextualTyping() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = (10)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& paren = static_cast<const ParenExpr&>(declX.initializer());

    const auto parenType = result.model.typeOf(paren);
    const auto innerType = result.model.typeOf(paren.inner());
    KAI_CHECK(parenType.has_value());
    KAI_CHECK(innerType.has_value());
    if (parenType && innerType) {
        KAI_CHECK(*parenType == Type::i64());
        KAI_CHECK(*innerType == Type::i64());
    }
}

// --- Negative parenthesized literal ---

void testNegativeParenthesizedLiteral() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i8 = -(128)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& unary = static_cast<const UnaryExpr&>(declX.initializer());
    const auto& paren = static_cast<const ParenExpr&>(unary.operand());

    const auto unaryType = result.model.typeOf(unary);
    const auto parenType = result.model.typeOf(paren);
    const auto literalType = result.model.typeOf(paren.inner());

    KAI_CHECK(unaryType.has_value());
    KAI_CHECK(parenType.has_value());
    KAI_CHECK(literalType.has_value());
    if (unaryType && parenType && literalType) {
        KAI_CHECK(*unaryType == Type::i8());
        KAI_CHECK(*parenType == Type::i8());
        KAI_CHECK(*literalType == Type::i8());
    }
}

// --- Annotation mismatch retains declared symbol type ---

void testAnnotationMismatchRetainsDeclaredSymbolType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::TypeMismatch);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i64());
    }
}

// --- Unknown annotation cascade ---

void testUnknownAnnotationCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: Foo = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }

    const auto initType = result.model.typeOf(declX.initializer());
    KAI_CHECK(initType.has_value());
    if (initType) {
        KAI_CHECK(*initType == Type::i32());
    }
}

// --- Deferred annotation ---

void testDeferredAnnotationRemainsUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(value: &i32) {\n    let x: &i32 = value\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isUnresolved());
    }

    const auto valueType = result.model.typeOf(declX.initializer());
    KAI_CHECK(valueType.has_value());
    if (valueType) {
        KAI_CHECK(valueType->isUnresolved());
    }
}

// --- Unannotated identifier inference ---

void testUnannotatedIdentifierInference() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(value: i64) {\n    let x = value\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i64());
    }
}

// --- Unknown identifier cascade ---

void testUnknownIdentifierCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& identifier = static_cast<const IdentifierExpr&>(declX.initializer());

    const auto identifierType = result.model.typeOf(identifier);
    KAI_CHECK(identifierType.has_value());
    if (identifierType) {
        KAI_CHECK(identifierType->isError());
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }
}

// --- String deferral ---

void testStringLiteralUnannotatedDeferral() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isUnresolved());
    }

    const auto stringType = result.model.typeOf(declX.initializer());
    KAI_CHECK(stringType.has_value());
    if (stringType) {
        KAI_CHECK(stringType->isUnresolved());
    }
}

void testStringLiteralAnnotatedDeferralNoMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i32 = \"hello\"\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }

    const auto stringType = result.model.typeOf(declX.initializer());
    KAI_CHECK(stringType.has_value());
    if (stringType) {
        KAI_CHECK(stringType->isUnresolved());
    }
}

// --- Deferred outer node still records type ---

void testDeferredMemberCalleeCallExprStillRecordsUnresolved() {
    // `foo()` where foo resolves to a user Function became a CONCRETE
    // call under Milestone 3 (see testDirectFunctionCallResultType and
    // friends below) - this M1 regression's "a still-deferred callee's
    // children are checked, but CallExpr stays Unresolved" premise now
    // uses a MemberExpr callee (still fully deferred: no method-call
    // semantics exist) to preserve that contract (Milestone 3 spec #19).
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32) {\n    let x = obj.method()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declX.initializer());
    const auto& member = static_cast<const MemberExpr&>(call.callee());

    const auto objectType = result.model.typeOf(member.object());
    const auto calleeType = result.model.typeOf(call.callee());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(objectType.has_value());
    KAI_CHECK(calleeType.has_value());
    KAI_CHECK(callType.has_value());
    if (objectType && calleeType && callType) {
        KAI_CHECK(*objectType == Type::i32());
        KAI_CHECK(calleeType->isUnresolved());
        KAI_CHECK(callType->isUnresolved());
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isUnresolved());
    }
}

void testDeferredOuterDoesNotPropagateChildError() {
    // `+` became an implemented operator in Milestone 2 (see
    // testImplementedOperatorPropagatesChildError below, where an Error
    // child on `+` now DOES propagate) - this M1 regression now uses
    // MemberExpr, still fully deferred, to prove the still-deferred rule
    // (child Error does NOT propagate to a still-deferred outer node).
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = unknown.field\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& member = static_cast<const MemberExpr&>(declX.initializer());

    const auto objectType = result.model.typeOf(member.object());
    const auto memberType = result.model.typeOf(member);
    KAI_CHECK(objectType.has_value());
    KAI_CHECK(memberType.has_value());
    if (objectType && memberType) {
        // The child genuinely errored...
        KAI_CHECK(objectType->isError());
        // ...but that error is NOT propagated to this still-deferred
        // outer node - it stays Unresolved, not Error.
        KAI_CHECK(memberType->isUnresolved());
        KAI_CHECK(!memberType->isError());
    }
}

void testImplementedOperatorPropagatesChildError() {
    // Milestone 2 spec #4/#26: for an IMPLEMENTED operator (here `+`), an
    // Error child DOES propagate to the operator's own result - and no
    // additional InvalidBinaryOperands is emitted on top of it.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = unknown + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(leftType.has_value());
    KAI_CHECK(binaryType.has_value());
    if (leftType && binaryType) {
        KAI_CHECK(leftType->isError());
        KAI_CHECK(binaryType->isError());
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }
}

void testUnresolvedOperandImplementedOperatorNoDiagnostic() {
    // Milestone 2 spec #4: an Unresolved operand makes an implemented
    // operator's own result Unresolved too, with no InvalidBinaryOperands.
    // A call to a resolved user Function became a CONCRETE CallExpr under
    // Milestone 3 (see testDirectFunctionCallResultType and friends
    // below), so this regression now uses a function whose declared
    // return type is itself still deferred (`&i32`, per Milestone 1) to
    // keep the CallExpr genuinely Unresolved.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = foo() + 1\n}\nfn foo() -> &i32 {}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(binaryType.has_value());
    if (binaryType) {
        KAI_CHECK(binaryType->isUnresolved());
    }
}
} // namespace

int main() {
    testExpressionMapIsNulloptBeforeTypeCheckerAndHasValueAfter();
    testDeferredExpressionStillRecordsUnresolvedEntry();

    testDefaultIntegerLiteralInfersI32();
    testDefaultFloatLiteralInfersF64();
    testDefaultBoolLiteralInfersBool();
    testDefaultCharLiteralInfersChar();
    testDefaultUnitLiteralInfersUnit();

    testIntegerContextualTypingAdaptsToDeclaredType();
    testIntegerOverflowProducesLiteralOutOfRange();

    testSignedBoundaryLiterals();
    testUnsignedNegativeLiteralOutOfRange();

    testFloatContextualTyping();
    testIntegerLiteralDoesNotAdaptToFloatContext();
    testFloatLiteralDoesNotAdaptToIntegerContext();

    testFixedLiteralTypesNeverAdapt();

    testParenthesizedContextualTyping();
    testNegativeParenthesizedLiteral();

    testAnnotationMismatchRetainsDeclaredSymbolType();
    testUnknownAnnotationCascade();
    testDeferredAnnotationRemainsUnresolved();

    testUnannotatedIdentifierInference();
    testUnknownIdentifierCascade();

    testStringLiteralUnannotatedDeferral();
    testStringLiteralAnnotatedDeferralNoMismatch();

    testDeferredMemberCalleeCallExprStillRecordsUnresolved();
    testDeferredOuterDoesNotPropagateChildError();
    testImplementedOperatorPropagatesChildError();
    testUnresolvedOperandImplementedOperatorNoDiagnostic();

    return kai::test::failureCount == 0 ? 0 : 1;
}
