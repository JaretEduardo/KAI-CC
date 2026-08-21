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
using kai::ast::AssignmentExpr;
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
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32, arr: i32, result: i32) {\n"
                                          "    obj.method()\n"
                                          "    arr[0]()\n"
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

// --- Milestone 4: valid mutable assignment ---

void testMutableAnnotatedReassignmentSuccess() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());

    const auto assignType = result.model.typeOf(assignment);
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(valueType.has_value());
    if (assignType && valueType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*valueType == Type::i64());
    }
}

void testMutableInferredReassignmentSuccess() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x = 0\n    x = 1\n}");
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
        // Assignment checking must never mutate a Symbol's recorded type.
        KAI_CHECK(result.model.symbol(*xId).type == Type::i32());
    }

    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

// --- Milestone 4: immutability ---

void testImmutableLocalReassignmentError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::AssignmentToImmutableBinding);
        KAI_CHECK(!error.expectedType.has_value());
        KAI_CHECK(!error.actualType.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testImmutableParameterReassignmentError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(x: i64) {\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

void testImmutabilityRelatedSpanPointsToDeclaration() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == declX.name().span);
        }
    }
}

void testImmutableTargetTypeMismatchDoesNotAlsoFire() {
    // examples/errors.kai's own regression shape, generalized: an
    // immutable target with an incompatible RHS type reports ONLY the
    // immutability error - never immutability + TypeMismatch together.
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x: i64 = 0\n    x = true\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

void testImmutableTargetStillChecksRhsForIndependentErrors() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    let x = 0\n    x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::AssignmentToImmutableBinding);
    }
}

// --- Milestone 4: concrete target / RHS compatibility ---

void testMutableWrongConcreteTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = true\n}");
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
            KAI_CHECK(*error.expectedType == Type::i64());
            KAI_CHECK(*error.actualType == Type::boolean());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testMutableI64ArithmeticContextualSubtree() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = 1 + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(valueType.has_value());
    if (valueType) {
        KAI_CHECK(*valueType == Type::i64());
    }
}

void testBoolTargetFromComparison() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut flag: bool = false\n    flag = 1 < 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    const auto valueType = result.model.typeOf(assignment.value());
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(valueType.has_value());
    if (assignType && valueType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*valueType == Type::boolean());
    }
}

void testU8ContextualFit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: u8 = 0\n    x = 255\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void testU8ContextualOverflow() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: u8 = 0\n    x = 256\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::LiteralOutOfRange);
        if (error.expectedType) {
            KAI_CHECK(*error.expectedType == Type::u8());
        }
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        // RHS became Error - no TypeMismatch is attempted on top.
        KAI_CHECK(assignType->isError());
    }
}

// --- Milestone 4: unknown / invalid targets ---

void testUnknownTargetNoExtraAssignmentDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testLiteralTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    1 = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Deterministic order: SemanticAnalyzer's UnknownIdentifier (RHS)
    // precedes TypeChecker's own InvalidAssignmentTarget.
    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::InvalidAssignmentTarget);
        KAI_CHECK(!result.model.errors()[1].expectedType.has_value());
        KAI_CHECK(!result.model.errors()[1].actualType.has_value());
        KAI_CHECK(!result.model.errors()[1].relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    KAI_CHECK(result.model.errors()[1].primarySpan == assignment.target().span());

    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testBinaryTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    1 + 2 = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& binaryTarget = static_cast<const BinaryExpr&>(assignment.target());

    const auto leftType = result.model.typeOf(binaryTarget.left());
    const auto rightType = result.model.typeOf(binaryTarget.right());
    KAI_CHECK(leftType.has_value());
    KAI_CHECK(rightType.has_value());
    if (leftType && rightType) {
        KAI_CHECK(*leftType == Type::i32());
        KAI_CHECK(*rightType == Type::i32());
    }
}

void testCallTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn foo() -> i64 {\n    return 1\n}\nfn f() {\n    foo() = 5\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto targetType = result.model.typeOf(assignment.target());
    KAI_CHECK(targetType.has_value());
    if (targetType) {
        // The call itself is still fully validated normally.
        KAI_CHECK(*targetType == Type::i64());
    }
}

void testFunctionIdentifierTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn foo() {}\nfn main() {\n    foo = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto targetType = result.model.typeOf(assignment.target());
    KAI_CHECK(targetType.has_value());
    if (targetType) {
        KAI_CHECK(targetType->isUnresolved());
    }
}

void testBuiltinIdentifierTargetInvalid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn main() {\n    print = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidAssignmentTarget);
    }
}

// --- Milestone 4: parenthesized targets ---

void testParenthesizedLocalTargetValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    (x) = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& paren = static_cast<const ParenExpr&>(assignment.target());

    const auto assignType = result.model.typeOf(assignment);
    const auto parenType = result.model.typeOf(paren);
    const auto identifierType = result.model.typeOf(paren.inner());
    KAI_CHECK(assignType.has_value() && parenType.has_value() && identifierType.has_value());
    if (assignType && parenType && identifierType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*parenType == Type::i64());
        KAI_CHECK(*identifierType == Type::i64());
    }
}

void testMultiplyParenthesizedLocalTargetValid() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    (((x))) = 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

// --- Milestone 4: target type Unresolved / Error recovery ---

void testTargetUnresolvedRhsNonErrorProducesUnit() {
    SourceManager sm;
    Checked result =
        analyzeAndCheck(sm, "fn f(value: &i32) {\n    mut y: &i32 = value\n    y = value\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(*assignType == Type::unit());
    }
}

void testTargetUnresolvedRhsErrorProducesError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(value: &i32) {\n    mut y: &i32 = value\n    y = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
    }
}

void testTargetErrorAlwaysError() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: Foo = 0\n    x = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType from the declaration - no new
    // diagnostic from the assignment itself.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());

    const auto valueType = result.model.typeOf(assignment.value());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(valueType.has_value());
    KAI_CHECK(assignType.has_value());
    if (valueType && assignType) {
        // RHS checked with no context (target type is Error) - I32 default.
        KAI_CHECK(*valueType == Type::i32());
        // Target Error is NOT treated like Unresolved - unconditionally Error.
        KAI_CHECK(assignType->isError());
    }
}

void testTargetErrorPreventsDownstreamBinaryOperandsCascade() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: Foo = 0\n    let y = (x = 1) + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Exactly the one UnknownType - the Error assignment propagates into
    // the binary expression with NO InvalidBinaryOperands cascade.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& binary = static_cast<const BinaryExpr&>(declY.initializer());
    const auto& paren = static_cast<const ParenExpr&>(binary.left());
    const auto& assignment = static_cast<const AssignmentExpr&>(paren.inner());

    const auto assignType = result.model.typeOf(assignment);
    const auto binaryType = result.model.typeOf(binary);
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(binaryType.has_value());
    if (assignType && binaryType) {
        KAI_CHECK(assignType->isError());
        KAI_CHECK(binaryType->isError());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

// --- Milestone 4: Member / Index deferred targets ---

void testMemberTargetUnresolvedNoDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32) {\n    obj.field = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isUnresolved());
    }
}

void testIndexTargetUnresolvedNoDiagnostic() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(arr: i32) {\n    arr[0] = 1\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isUnresolved());
    }
}

void testDeferredMemberTargetRhsErrorStillUnresolved() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f(obj: i32) {\n    obj.field = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        // Deferred target FORM - a child Error does NOT propagate here,
        // unlike a valid mutable target (see testTargetUnresolvedRhsErrorProducesError).
        KAI_CHECK(assignType->isUnresolved());
        KAI_CHECK(!assignType->isError());
    }
}

// --- Milestone 4: expression-position / operator integration ---

void testLetYEqualsParenAssignInfersUnit() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    let y = (x = 1)\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto& paren = static_cast<const ParenExpr&>(declY.initializer());
    const auto& assignment = static_cast<const AssignmentExpr&>(paren.inner());

    const auto assignType = result.model.typeOf(assignment);
    const auto parenType = result.model.typeOf(paren);
    KAI_CHECK(assignType.has_value());
    KAI_CHECK(parenType.has_value());
    if (assignType && parenType) {
        KAI_CHECK(*assignType == Type::unit());
        KAI_CHECK(*parenType == Type::unit());
    }

    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type == Type::unit());
    }
}

void testAssignmentPlusArithmeticInvalidBinaryOperands() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    let y = (x = 1) + 2\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::InvalidBinaryOperands);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto yId = result.model.declarationSymbol(declY.name());
    KAI_CHECK(yId.has_value());
    if (yId) {
        KAI_CHECK(result.model.symbol(*yId).type.isError());
    }
}

void testChainedAssignmentTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n"
                                          "    mut x: i64 = 0\n    mut y: i64 = 0\n    mut z: i64 = 0\n"
                                          "    x = y = z\n}");
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

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[3]);
    const auto& outer = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto& inner = static_cast<const AssignmentExpr&>(outer.value());

    const auto innerType = result.model.typeOf(inner);
    const auto outerType = result.model.typeOf(outer);
    KAI_CHECK(innerType.has_value());
    KAI_CHECK(outerType.has_value());
    if (innerType && outerType) {
        KAI_CHECK(*innerType == Type::unit());
        KAI_CHECK(outerType->isError());
    }
}

void testRhsErrorProducesAssignmentErrorNoTypeMismatch() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm, "fn f() {\n    mut x: i64 = 0\n    x = unknown\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& assignStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& assignment = static_cast<const AssignmentExpr&>(assignStmt.expr());
    const auto assignType = result.model.typeOf(assignment);
    KAI_CHECK(assignType.has_value());
    if (assignType) {
        KAI_CHECK(assignType->isError());
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

    testDeferredMemberCalleeCallExprStillRecordsUnresolved();
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

    // --- Milestone 3: Function Calls ---

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

    // --- Milestone 4: Assignment and Binding Mutability ---

    testMutableAnnotatedReassignmentSuccess();
    testMutableInferredReassignmentSuccess();

    testImmutableLocalReassignmentError();
    testImmutableParameterReassignmentError();
    testImmutabilityRelatedSpanPointsToDeclaration();
    testImmutableTargetTypeMismatchDoesNotAlsoFire();
    testImmutableTargetStillChecksRhsForIndependentErrors();

    testMutableWrongConcreteTypeMismatch();
    testMutableI64ArithmeticContextualSubtree();
    testBoolTargetFromComparison();
    testU8ContextualFit();
    testU8ContextualOverflow();

    testUnknownTargetNoExtraAssignmentDiagnostic();
    testLiteralTargetInvalid();
    testBinaryTargetInvalid();
    testCallTargetInvalid();
    testFunctionIdentifierTargetInvalid();
    testBuiltinIdentifierTargetInvalid();

    testParenthesizedLocalTargetValid();
    testMultiplyParenthesizedLocalTargetValid();

    testTargetUnresolvedRhsNonErrorProducesUnit();
    testTargetUnresolvedRhsErrorProducesError();
    testTargetErrorAlwaysError();
    testTargetErrorPreventsDownstreamBinaryOperandsCascade();

    testMemberTargetUnresolvedNoDiagnostic();
    testIndexTargetUnresolvedNoDiagnostic();
    testDeferredMemberTargetRhsErrorStillUnresolved();

    testLetYEqualsParenAssignInfersUnit();
    testAssignmentPlusArithmeticInvalidBinaryOperands();
    testChainedAssignmentTypeMismatch();
    testRhsErrorProducesAssignmentErrorNoTypeMismatch();

    testFullTraversalRecordsNestedExpressionTypes();

    return kai::test::failureCount == 0 ? 0 : 1;
}
