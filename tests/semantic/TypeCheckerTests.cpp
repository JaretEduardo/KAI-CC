#include "kai/semantic/TypeChecker.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::ArrayLiteralExpr;
using kai::ast::BinaryExpr;
using kai::ast::CallExpr;
using kai::ast::ErrorPropagationExpr;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
using kai::ast::IndexExpr;
using kai::ast::MemberExpr;
using kai::ast::ParenExpr;
using kai::ast::UnaryExpr;
using kai::ast::VarDeclStmt;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticErrorKind;
using kai::semantic::SemanticModel;
using kai::semantic::Type;
using kai::semantic::TypeChecker;

namespace {

// Mirrors SemanticAnalyzerTests.cpp's own Analyzed bundle, extended with
// the TypeChecker pass run on top of SemanticAnalyzer's output - the
// exact pipeline Milestone 1's spec requires: SourceManager -> Parser ->
// SemanticAnalyzer -> SemanticModel -> TypeChecker -> query mutated
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
    }

    return Checked{std::move(parsed), std::move(model)};
}

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

void testDeferredCallExprStillRecordsTypes() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = foo()\n}\nfn foo() {}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(declX.initializer());

    const auto calleeType = result.model.typeOf(call.callee());
    const auto callType = result.model.typeOf(call);
    KAI_CHECK(calleeType.has_value());
    KAI_CHECK(callType.has_value());
    if (calleeType && callType) {
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
    // Milestone 2 spec #4: an Unresolved operand (here a CallExpr result,
    // still deferred per M1) makes an implemented operator's own result
    // Unresolved too, with no InvalidBinaryOperands.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = foo() + 1\n}\nfn foo() {}");
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

// --- Milestone 2: Unary ---

void testNegateSignedIntegerReturnsOperandType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32, y: i64) {\n    let a = -x\n    let b = -y\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i32());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::i64());
    }
}

void testNegateFloatReturnsOperandType() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(z: f32) {\n    let c = -z\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto cId = result.model.declarationSymbol(declC.name());
    KAI_CHECK(cId.has_value());
    if (cId) {
        KAI_CHECK(result.model.symbol(*cId).type == Type::f32());
    }
}

void testNegateUnsignedIntegerInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: u8) {\n    let y = -x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

void testNegateBoolInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(flag: bool) {\n    let y = -flag\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }
}

void testNotBoolReturnsBool() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(flag: bool) {\n    let a = !flag\n    let b = !true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::boolean());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::boolean());
    }
}

void testNotIntegerInvalidUnaryOperand() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {\n    let y = !x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidUnaryOperand);
        KAI_CHECK(error.actualType.has_value());
        if (error.actualType) {
            KAI_CHECK(*error.actualType == Type::i32());
        }
    }
}

void testNegativeFloatLiteralRegression() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: f32 = -1.5\n    let y = -1.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto xType = result.model.typeOf(declX.initializer());
    KAI_CHECK(xType.has_value());
    if (xType) {
        KAI_CHECK(*xType == Type::f32());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::f64());
    }
}

// --- Milestone 2: Arithmetic ---

void testArithmeticSameTypeSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: i32, b: i32, c: u64, d: u64, e: f32, g: f32) {\n"
            "    let x = a + b\n    let y = c * d\n    let z = e / g\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& declZ = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);

    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    const auto zId = result.model.declarationSymbol(declZ.name());
    KAI_CHECK(xId.has_value() && yId.has_value() && zId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::u64());
    }
    if (zId) {
        KAI_CHECK(result.model.symbol(*zId).type == Type::f32());
    }
}

void testArithmeticMixedTypesInvalidBinaryOperands() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(a: i32, b: i64, c: f32) {\n    let x = a + b\n    let y = a + c\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(!error.actualType.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(xId.has_value() && yId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isError());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 2: Modulo ---

void testModuloIntegerSucceeds() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(a: i32, b: i32, c: u64, d: u64) {\n    let x = a % b\n    let y = c % d\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::u64());
    }
}

void testModuloFloatInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: f32, b: f32) {\n    let x = a % b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Ordering ---

void testOrderingNumericSucceedsBool() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: i32, b: i32, c: u64, d: u64, e: f32, g: f32) {\n"
            "    let x = a < b\n    let y = c >= d\n    let z = e > g\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }
}

void testOrderingRejectsInvalidOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(a: bool, b: bool, c: char, d: char, e: i32, g: i64, h: i32, i: f32) {\n"
            "    let w = a < b\n    let x = c < d\n    let y = e < g\n    let z = h < i\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 4);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Logical ---

void testLogicalBoolSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: bool, b: bool) {\n    let x = true && false\n    let y = a || b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    const auto yId = result.model.declarationSymbol(declY.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::boolean());
    }
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::boolean());
    }
}

void testLogicalNonBoolInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i32, b: bool) {\n    let x = a && b\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: Equality ---

void testEqualitySameTypeSucceeds() {
    SourceManager sm;
    Checked result = analyzeAndCheck(
        sm, "fn f(p: i32, q: i32, r: f64, s: f64, a: bool, b: bool, c: char, d: char) {\n"
            "    let w = p == q\n    let x = r != s\n    let y = a == b\n    let z = c != d\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }
}

void testEqualityRejectsCrossTypeAndUnit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(a: i32, b: i64, c: f64, d: bool) {\n"
                                          "    let w = a == b\n    let x = a == c\n"
                                          "    let y = d == a\n    let z = () == ()\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 4);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::InvalidBinaryOperands);
    }
}

// --- Milestone 2: contextual arithmetic typing ---

void testWholeExpressionArithmeticContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(leftType.has_value() && rightType.has_value() && binaryType.has_value());
    if (leftType && rightType && binaryType) {
        KAI_CHECK(*leftType == Type::i64());
        KAI_CHECK(*rightType == Type::i64());
        KAI_CHECK(*binaryType == Type::i64());
    }
}

void testFixedSiblingArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    let a = x + 1\n    let b = 1 + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& binaryA = static_cast<const BinaryExpr&>(declA.initializer());
    const auto& binaryB = static_cast<const BinaryExpr&>(declB.initializer());

    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::i64());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::i64());
    }

    const auto literalInA = result.model.typeOf(binaryA.right());
    const auto literalInB = result.model.typeOf(binaryB.left());
    KAI_CHECK(literalInA.has_value() && literalInB.has_value());
    if (literalInA && literalInB) {
        KAI_CHECK(*literalInA == Type::i64());
        KAI_CHECK(*literalInB == Type::i64());
    }
}

void testNestedArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    let c = x + (1 + 2)\n    let d = (1 + 2) + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declD = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    const auto cId = result.model.declarationSymbol(declC.name());
    const auto dId = result.model.declarationSymbol(declD.name());
    if (cId) {
        KAI_CHECK(result.model.symbol(*cId).type == Type::i64());
    }
    if (dId) {
        KAI_CHECK(result.model.symbol(*dId).type == Type::i64());
    }

    const auto& binaryC = static_cast<const BinaryExpr&>(declC.initializer());
    const auto& parenC = static_cast<const ParenExpr&>(binaryC.right());
    const auto& innerC = static_cast<const BinaryExpr&>(parenC.inner());

    const auto& binaryD = static_cast<const BinaryExpr&>(declD.initializer());
    const auto& parenD = static_cast<const ParenExpr&>(binaryD.left());
    const auto& innerD = static_cast<const BinaryExpr&>(parenD.inner());

    for (const BinaryExpr* inner : {&innerC, &innerD}) {
        const auto parenType = result.model.typeOf(*inner);
        const auto leftType = result.model.typeOf(inner->left());
        const auto rightType = result.model.typeOf(inner->right());
        KAI_CHECK(parenType.has_value() && leftType.has_value() && rightType.has_value());
        if (parenType && leftType && rightType) {
            KAI_CHECK(*parenType == Type::i64());
            KAI_CHECK(*leftType == Type::i64());
            KAI_CHECK(*rightType == Type::i64());
        }
    }
    const auto parenCType = result.model.typeOf(parenC);
    const auto parenDType = result.model.typeOf(parenD);
    KAI_CHECK(parenCType.has_value() && parenDType.has_value());
    if (parenCType && parenDType) {
        KAI_CHECK(*parenCType == Type::i64());
        KAI_CHECK(*parenDType == Type::i64());
    }
}

void testFloatSiblingArithmeticContextBothOrders() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: f32) {\n    let a = x + 1.5\n    let b = 1.5 + x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type == Type::f32());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type == Type::f32());
    }
}

void testCrossFamilyArithmeticRejectsAdaptation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: f64) {\n    let y = x + 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declY.initializer());

    const auto literalType = result.model.typeOf(binary.right());
    KAI_CHECK(literalType.has_value());
    if (literalType) {
        KAI_CHECK(*literalType == Type::i32());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 2: whole-result context must not contaminate comparison/equality ---

void testComparisonOperandsIgnoreWholeExpressionContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 < 2\n}");
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

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(leftType.has_value() && rightType.has_value() && binaryType.has_value());
    if (leftType && rightType && binaryType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
        KAI_CHECK(*binaryType == Type::boolean());
    }
}

void testEqualityOperandsIgnoreWholeExpressionContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 1 == 2\n}");
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

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto leftType = result.model.typeOf(binary.left());
    const auto rightType = result.model.typeOf(binary.right());
    KAI_CHECK(leftType.has_value() && rightType.has_value());
    if (leftType && rightType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
    }
}

void testComparisonOverflowRegressionIgnoresOuterContext() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 2147483648 < 2147483649\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Outer i64 must NOT contextualize the comparison's operands - both
    // integer literals use the default I32 target and overflow it.
    KAI_CHECK(result.model.errors().size() == 2);
    for (const auto& error : result.model.errors()) {
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i32());
        }
        KAI_CHECK(!error.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& binary = static_cast<const BinaryExpr&>(declX.initializer());

    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(binaryType.has_value());
    if (binaryType) {
        // Left operand errored (LiteralOutOfRange) -> propagates to Error,
        // never a spurious InvalidBinaryOperands/TypeMismatch on top.
        KAI_CHECK(binaryType->isError());
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::i64());
    }
}

void testComparisonEqualitySiblingAnchor() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n"
                                          "    let a = x < 1\n    let b = 1 < x\n"
                                          "    let c = x == 1\n    let d = 1 == x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    for (const auto& stmt : fn.body().statements()) {
        const auto& varDecl = static_cast<const VarDeclStmt&>(*stmt);
        const auto id = result.model.declarationSymbol(varDecl.name());
        KAI_CHECK(id.has_value());
        if (id) {
            KAI_CHECK(result.model.symbol(*id).type == Type::boolean());
        }
    }

    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& declC = static_cast<const VarDeclStmt&>(*fn.body().statements()[2]);
    const auto& declD = static_cast<const VarDeclStmt&>(*fn.body().statements()[3]);
    const auto& binaryA = static_cast<const BinaryExpr&>(declA.initializer());
    const auto& binaryB = static_cast<const BinaryExpr&>(declB.initializer());
    const auto& binaryC = static_cast<const BinaryExpr&>(declC.initializer());
    const auto& binaryD = static_cast<const BinaryExpr&>(declD.initializer());

    const auto literalA = result.model.typeOf(binaryA.right());
    const auto literalB = result.model.typeOf(binaryB.left());
    const auto literalC = result.model.typeOf(binaryC.right());
    const auto literalD = result.model.typeOf(binaryD.left());
    for (const auto& literalType : {literalA, literalB, literalC, literalD}) {
        KAI_CHECK(literalType.has_value());
        if (literalType) {
            KAI_CHECK(*literalType == Type::i64());
        }
    }
}

// --- Milestone 2: annotation-span provenance through arithmetic ---

void testArithmeticAnnotationSpanPropagation() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: u8 = 300 + 1\n}");
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
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::u8());
    }
}

void testArithmeticSiblingAnchorDoesNotFabricateAnnotationSpan() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i8) {\n    let y = x + 200\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        // The anchor came from sibling `x`, not an explicit annotation -
        // no relatedSpan is fabricated for it.
        KAI_CHECK(!error.relatedSpan.has_value());
        KAI_CHECK(error.expectedType.has_value());
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::i8());
        }
    }
}

// --- Milestone 2: Range / Ref / RefMut remain fully deferred ---

void testRangeRemainsUnresolvedNoOperatorDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 1..2.5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type.isUnresolved());
    }
}

void testRefAndRefMutRemainUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i32) {\n    let a = &x\n    let b = &mut x\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto aId = result.model.declarationSymbol(declA.name());
    const auto bId = result.model.declarationSymbol(declB.name());
    if (aId) {
        KAI_CHECK(result.model.symbol(*aId).type.isUnresolved());
    }
    if (bId) {
        KAI_CHECK(result.model.symbol(*bId).type.isUnresolved());
    }
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

    testDeferredCallExprStillRecordsTypes();
    testDeferredOuterDoesNotPropagateChildError();
    testImplementedOperatorPropagatesChildError();
    testUnresolvedOperandImplementedOperatorNoDiagnostic();

    // --- Milestone 2: Primitive Operators ---

    testNegateSignedIntegerReturnsOperandType();
    testNegateFloatReturnsOperandType();
    testNegateUnsignedIntegerInvalidUnaryOperand();
    testNegateBoolInvalidUnaryOperand();
    testNotBoolReturnsBool();
    testNotIntegerInvalidUnaryOperand();
    testNegativeFloatLiteralRegression();

    testArithmeticSameTypeSucceeds();
    testArithmeticMixedTypesInvalidBinaryOperands();

    testModuloIntegerSucceeds();
    testModuloFloatInvalidBinaryOperands();

    testOrderingNumericSucceedsBool();
    testOrderingRejectsInvalidOperands();

    testLogicalBoolSucceeds();
    testLogicalNonBoolInvalidBinaryOperands();

    testEqualitySameTypeSucceeds();
    testEqualityRejectsCrossTypeAndUnit();

    testWholeExpressionArithmeticContext();
    testFixedSiblingArithmeticContextBothOrders();
    testNestedArithmeticContextBothOrders();
    testFloatSiblingArithmeticContextBothOrders();
    testCrossFamilyArithmeticRejectsAdaptation();

    testComparisonOperandsIgnoreWholeExpressionContext();
    testEqualityOperandsIgnoreWholeExpressionContext();
    testComparisonOverflowRegressionIgnoresOuterContext();
    testComparisonEqualitySiblingAnchor();

    testArithmeticAnnotationSpanPropagation();
    testArithmeticSiblingAnchorDoesNotFabricateAnnotationSpan();

    testRangeRemainsUnresolvedNoOperatorDiagnostic();
    testRefAndRefMutRemainUnresolved();

    testFullTraversalRecordsNestedExpressionTypes();

    return kai::test::failureCount == 0 ? 0 : 1;
}
