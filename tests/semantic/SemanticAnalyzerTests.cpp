#include "kai/semantic/SemanticAnalyzer.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <cstddef>
#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::ArrayLiteralExpr;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::CallExpr;
using kai::ast::ErrorPropagationExpr;
using kai::ast::ExprKind;
using kai::ast::ExprStmt;
using kai::ast::ForStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
using kai::ast::IfStmt;
using kai::ast::IndexExpr;
using kai::ast::MemberExpr;
using kai::ast::ParenExpr;
using kai::ast::ReturnStmt;
using kai::ast::UnaryExpr;
using kai::ast::VarDeclStmt;
using kai::ast::WhileStmt;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::FunctionSignature;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticErrorKind;
using kai::semantic::SemanticModel;
using kai::semantic::Symbol;
using kai::semantic::SymbolId;
using kai::semantic::SymbolKind;
using kai::semantic::Type;

namespace {

// Bundles a parsed SourceFile (or parse failure) together with the
// SemanticModel analyzed from it. ast::SourceFile is movable (unlike
// SourceManager, which is neither copyable nor movable), so this bundle
// can be returned by value and stored as one local in each test - the
// SourceFile and the SemanticModel that points into it stay alive
// together for exactly as long as the bundle itself does, satisfying
// SemanticModel's documented lifetime contract. `sm` is supplied by the
// caller and must already be alive for at least as long as this bundle.
struct Analyzed {
    ParseResult<kai::ast::SourceFile> parsed;
    SemanticModel model;
};

Analyzed analyze(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();

    SemanticModel model;
    if (parsed.has_value()) {
        SemanticAnalyzer analyzer(sm);
        model = analyzer.analyze(*parsed);
    }

    return Analyzed{std::move(parsed), std::move(model)};
}

// --- Primitive signatures ---

void testPrimitiveParameterAndReturnSignature() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn add(\n"
                                   "    a: i8,\n"
                                   "    b: i16,\n"
                                   "    c: i32,\n"
                                   "    d: i64,\n"
                                   "    e: u8,\n"
                                   "    f: u16,\n"
                                   "    g: u32,\n"
                                   "    h: u64,\n"
                                   "    i: f32,\n"
                                   "    j: f64,\n"
                                   "    k: bool,\n"
                                   "    l: char\n"
                                   ") -> i32 {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto id = result.model.declarationSymbol(fn.name());
    KAI_CHECK(id.has_value());
    if (!id) {
        return;
    }

    const Symbol& symbol = result.model.symbol(*id);
    KAI_CHECK(symbol.kind == SymbolKind::Function);
    KAI_CHECK(symbol.signature.has_value());
    if (!symbol.signature) {
        return;
    }

    const FunctionSignature& signature = *symbol.signature;
    const Type expected[] = {
        Type::i8(),  Type::i16(), Type::i32(),      Type::i64(),      Type::u8(),  Type::u16(),
        Type::u32(), Type::u64(), Type::f32(),      Type::f64(),      Type::boolean(), Type::character(),
    };

    KAI_CHECK(signature.parameterTypes.size() == 12);
    for (std::size_t index = 0; index < 12 && index < signature.parameterTypes.size(); ++index) {
        KAI_CHECK(signature.parameterTypes[index] == expected[index]);
    }
    KAI_CHECK(signature.returnType == Type::i32());
}

// --- Unit return ---

void testMissingAndExplicitUnitReturnBothResolveToUnit() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {}\nfn g() -> () {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& f = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& g = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);

    // The AST alone still preserves the syntactic distinction.
    KAI_CHECK(f.returnType() == nullptr);
    KAI_CHECK(g.returnType() != nullptr);

    const auto fId = result.model.declarationSymbol(f.name());
    const auto gId = result.model.declarationSymbol(g.name());
    KAI_CHECK(fId.has_value());
    KAI_CHECK(gId.has_value());
    if (!fId || !gId) {
        return;
    }

    KAI_CHECK(result.model.symbol(*fId).signature->returnType == Type::unit());
    KAI_CHECK(result.model.symbol(*gId).signature->returnType == Type::unit());
}

// --- Symbol::name / declaredAt for a real source declaration ---

void testSourceFunctionSymbolHasSourceNameAndDeclaredAtSpan() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn add() {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto id = result.model.declarationSymbol(fn.name());
    KAI_CHECK(id.has_value());
    if (!id) {
        return;
    }

    const Symbol& symbol = result.model.symbol(*id);
    KAI_CHECK(symbol.name == "add");
    KAI_CHECK(symbol.declaredAt.has_value());
    if (symbol.declaredAt) {
        KAI_CHECK(*symbol.declaredAt == fn.name().span);
    }
}

// --- Unknown type ---

void testUnknownParameterTypeProducesOneError() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: Foo) {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::UnknownType);
        KAI_CHECK(sm.text(error.primarySpan) == "Foo");
        KAI_CHECK(!error.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto id = result.model.declarationSymbol(fn.name());
    KAI_CHECK(id.has_value());
    if (!id) {
        return;
    }
    KAI_CHECK(result.model.symbol(*id).signature->parameterTypes[0] == Type::error());
}

// --- Deferred TypeSyntax shapes ---

void testDeferredTypeShapesResolveToUnresolvedWithNoErrors() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(\n"
                                   "    a: &i32,\n"
                                   "    b: [i32],\n"
                                   "    c: [i32; 4],\n"
                                   "    d: Result<i32, E>\n"
                                   ") {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // No errors solely from these deferred forms - not even for the
    // NamedTypeSyntax identifiers nested inside Result<i32, E>.
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto id = result.model.declarationSymbol(fn.name());
    KAI_CHECK(id.has_value());
    if (!id) {
        return;
    }

    const FunctionSignature& signature = *result.model.symbol(*id).signature;
    KAI_CHECK(signature.parameterTypes.size() == 4);
    for (const Type& type : signature.parameterTypes) {
        KAI_CHECK(type.isUnresolved());
        KAI_CHECK(!type.isError());
    }
}

// --- Duplicate top-level functions ---

void testDuplicateTopLevelFunctionProducesExactlyOneError() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn a() {}\nfn a() {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& first = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& second = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::DuplicateSymbol);
        KAI_CHECK(error.primarySpan == second.name().span);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == first.name().span);
        }
    }

    // Both declarations still receive their own Symbol/declarationSymbol
    // mapping, and the two SymbolIds are distinct - only file-scope name
    // lookup (analyzer-internal, not part of SemanticModel) treats the
    // first as the winner.
    const auto firstId = result.model.declarationSymbol(first.name());
    const auto secondId = result.model.declarationSymbol(second.name());
    KAI_CHECK(firstId.has_value());
    KAI_CHECK(secondId.has_value());
    if (firstId && secondId) {
        KAI_CHECK(!(*firstId == *secondId));
        KAI_CHECK(result.model.symbol(*firstId).declaredAt == first.name().span);
        KAI_CHECK(result.model.symbol(*secondId).declaredAt == second.name().span);
    }
}

// --- Multiple independent errors, deterministic order ---

void testMultipleErrorsAreCollectedInDeterministicSourceOrder() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn a(x: Foo) {}\nfn a(y: Bar) {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // Deterministic order: the first declaration's own signature is
    // resolved (UnknownType Foo) before the second declaration is even
    // reached; the second declaration's duplicate-name check runs before
    // its own signature is resolved (DuplicateSymbol, then UnknownType
    // Bar) - see SemanticAnalyzer.cpp's collectFunctionDecl() comment.
    KAI_CHECK(result.model.errors().size() == 3);
    if (result.model.errors().size() != 3) {
        return;
    }

    KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
    KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "Foo");

    KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::DuplicateSymbol);

    KAI_CHECK(result.model.errors()[2].kind == SemanticErrorKind::UnknownType);
    KAI_CHECK(sm.text(result.model.errors()[2].primarySpan) == "Bar");
}

// --- Body identifier uses are now resolved (Phase 3B) ---

void testUnresolvedBodyIdentifierProducesUnknownIdentifier() {
    // fn f() { unknown_name } - supersedes the old Phase-3A-era boundary
    // test of the same source, whose premise ("bodies are structurally
    // walked but no identifier use is ever resolved") is exactly what
    // this phase overturns.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    unknown_name\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(error.primarySpan) == "unknown_name");
        KAI_CHECK(!error.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Identifier);
    const auto& identifierExpr = static_cast<const IdentifierExpr&>(exprStmt.expr());

    // No fabricated Symbol, no resolution entry for an unresolved use.
    KAI_CHECK(!result.model.resolution(identifierExpr).has_value());
}

// --- Parameter symbols (Phase 3A) ---

void testParameterSymbolsHaveCorrectFields() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32, b: bool) {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto fnId = result.model.declarationSymbol(fn.name());
    KAI_CHECK(fnId.has_value());
    if (!fnId) {
        return;
    }
    const FunctionSignature& signature = *result.model.symbol(*fnId).signature;

    const auto aId = result.model.declarationSymbol(fn.params()[0].name);
    const auto bId = result.model.declarationSymbol(fn.params()[1].name);
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());
    if (!aId || !bId) {
        return;
    }

    const Symbol& a = result.model.symbol(*aId);
    KAI_CHECK(a.kind == SymbolKind::Parameter);
    KAI_CHECK(a.name == "a");
    KAI_CHECK(a.declaredAt.has_value());
    KAI_CHECK(a.declaredAt == fn.params()[0].name.span);
    KAI_CHECK(!a.isMutable);
    KAI_CHECK(a.type == Type::i32());
    KAI_CHECK(a.type == signature.parameterTypes[0]);

    const Symbol& b = result.model.symbol(*bId);
    KAI_CHECK(b.kind == SymbolKind::Parameter);
    KAI_CHECK(b.name == "b");
    KAI_CHECK(b.declaredAt == fn.params()[1].name.span);
    KAI_CHECK(!b.isMutable);
    KAI_CHECK(b.type == Type::boolean());
    KAI_CHECK(b.type == signature.parameterTypes[1]);
}

void testDuplicateParametersProduceOneErrorAndTwoSymbols() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32, a: i32) {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::DuplicateSymbol);
        KAI_CHECK(error.primarySpan == fn.params()[1].name.span);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == fn.params()[0].name.span);
        }
    }

    const auto firstId = result.model.declarationSymbol(fn.params()[0].name);
    const auto secondId = result.model.declarationSymbol(fn.params()[1].name);
    KAI_CHECK(firstId.has_value());
    KAI_CHECK(secondId.has_value());
    if (firstId && secondId) {
        KAI_CHECK(!(*firstId == *secondId));
    }
}

// --- Local symbols (Phase 3A) ---

void testLocalSymbolsHaveCorrectFieldsAndMutability() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    mut y: i64 = 2\n}");

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
    KAI_CHECK(xId.has_value());
    KAI_CHECK(yId.has_value());
    if (!xId || !yId) {
        return;
    }

    const Symbol& x = result.model.symbol(*xId);
    KAI_CHECK(x.kind == SymbolKind::Local);
    KAI_CHECK(x.name == "x");
    KAI_CHECK(!x.isMutable);
    // No annotation, no inference in this phase: Unresolved, not I32.
    KAI_CHECK(x.type.isUnresolved());

    const Symbol& y = result.model.symbol(*yId);
    KAI_CHECK(y.kind == SymbolKind::Local);
    KAI_CHECK(y.name == "y");
    KAI_CHECK(y.isMutable);
    KAI_CHECK(y.type == Type::i64());
}

void testDuplicateLocalsProduceOneErrorAndTwoSymbols() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    let x = 2\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& first = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& second = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::DuplicateSymbol);
        KAI_CHECK(error.primarySpan == second.name().span);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == first.name().span);
        }
    }

    const auto firstId = result.model.declarationSymbol(first.name());
    const auto secondId = result.model.declarationSymbol(second.name());
    KAI_CHECK(firstId.has_value());
    KAI_CHECK(secondId.has_value());
    if (firstId && secondId) {
        KAI_CHECK(!(*firstId == *secondId));
    }
}

void testParameterAndBodyLocalCollisionIsDuplicateSymbol() {
    // fn f(x: i32) { let x = 1 } - parameter and the outermost body
    // block share ONE lexical scope: this is a same-scope duplicate, not
    // shadowing.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: i32) {\n    let x = 1\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& localX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::DuplicateSymbol);
        KAI_CHECK(error.primarySpan == localX.name().span);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == fn.params()[0].name.span);
        }
    }

    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    const auto localId = result.model.declarationSymbol(localX.name());
    KAI_CHECK(paramId.has_value());
    KAI_CHECK(localId.has_value());
    if (paramId && localId) {
        KAI_CHECK(!(*paramId == *localId));
    }
}

// --- Nested scope shadowing (Phase 3A) ---

void testNestedIfBlockShadowsParameter() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: i32) {\n    if true {\n        let x = 1\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    const auto& innerX = static_cast<const VarDeclStmt&>(*ifStmt.branches()[0].body->statements()[0]);

    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    const auto innerId = result.model.declarationSymbol(innerX.name());
    KAI_CHECK(paramId.has_value());
    KAI_CHECK(innerId.has_value());
    if (paramId && innerId) {
        KAI_CHECK(!(*paramId == *innerId));
    }
}

void testSiblingIfElseBranchesDoNotCollide() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    if true {\n        let x = 1\n    } else {\n        let x = 2\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    const auto& thenX = static_cast<const VarDeclStmt&>(*ifStmt.branches()[0].body->statements()[0]);
    const auto& elseX = static_cast<const VarDeclStmt&>(*ifStmt.elseClause()->body->statements()[0]);

    const auto thenId = result.model.declarationSymbol(thenX.name());
    const auto elseId = result.model.declarationSymbol(elseX.name());
    KAI_CHECK(thenId.has_value());
    KAI_CHECK(elseId.has_value());
    if (thenId && elseId) {
        KAI_CHECK(!(*thenId == *elseId));
    }
}

void testWhileBodyShadowsOuterLocal() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    while true {\n        let x = 2\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[1]);
    const auto& innerX = static_cast<const VarDeclStmt&>(*whileStmt.body().statements()[0]);

    const auto outerId = result.model.declarationSymbol(outerX.name());
    const auto innerId = result.model.declarationSymbol(innerX.name());
    KAI_CHECK(outerId.has_value());
    KAI_CHECK(innerId.has_value());
    if (outerId && innerId) {
        KAI_CHECK(!(*outerId == *innerId));
    }
}

// --- For-loop variable (Phase 3A) ---

void testForVariableSymbol() {
    // "values" is now a genuinely resolved name (Phase 3B analyzes the
    // iterable): predeclared here so this test stays focused on the
    // for-variable Symbol itself rather than an incidental
    // UnknownIdentifier from an undeclared iterable.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let values = 1\n    for item in values {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);

    const auto itemId = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(itemId.has_value());
    if (!itemId) {
        return;
    }
    const Symbol& item = result.model.symbol(*itemId);
    KAI_CHECK(item.kind == SymbolKind::Local);
    KAI_CHECK(item.name == "item");
    KAI_CHECK(!item.isMutable);
    KAI_CHECK(item.type.isUnresolved());
    KAI_CHECK(item.declaredAt == forStmt.variable().span);
}

void testForBodyCollisionWithLoopVariable() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let values = 1\n    for item in values {\n        let item = 1\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);
    const auto& bodyItem = static_cast<const VarDeclStmt&>(*forStmt.body().statements()[0]);

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::DuplicateSymbol);
        KAI_CHECK(error.primarySpan == bodyItem.name().span);
        KAI_CHECK(error.relatedSpan.has_value());
        if (error.relatedSpan) {
            KAI_CHECK(*error.relatedSpan == forStmt.variable().span);
        }
    }

    const auto loopVarId = result.model.declarationSymbol(forStmt.variable());
    const auto bodyVarId = result.model.declarationSymbol(bodyItem.name());
    KAI_CHECK(loopVarId.has_value());
    KAI_CHECK(bodyVarId.has_value());
    if (loopVarId && bodyVarId) {
        KAI_CHECK(!(*loopVarId == *bodyVarId));
    }
}

void testForVariableShadowsOuterLocal() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let item = 1\n    let values = 2\n    for item in values {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerItem = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[2]);

    const auto outerId = result.model.declarationSymbol(outerItem.name());
    const auto loopVarId = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(outerId.has_value());
    KAI_CHECK(loopVarId.has_value());
    if (outerId && loopVarId) {
        KAI_CHECK(!(*outerId == *loopVarId));
    }
}

// --- Local annotation resolution reuses the existing type resolver ---

void testLocalUnknownAnnotationProducesUnknownTypeNotUnknownIdentifier() {
    SourceManager sm;
    // "value" is a parameter here (not a bare undeclared name) so this
    // test stays isolated to annotation resolution: Phase 3B does
    // resolve the initializer now, and an incidentally-undeclared
    // "value" would add an unrelated UnknownIdentifier into the count.
    Analyzed result = analyze(sm, "fn f(value: i32) {\n    let x: Foo = value\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    // Exactly one error, and it is UnknownType - the initializer
    // "value" resolves cleanly to the parameter, so it contributes no
    // UnknownIdentifier of its own.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownType);
        KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "Foo");
    }

    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());
    if (xId) {
        KAI_CHECK(result.model.symbol(*xId).type == Type::error());
    }
}

// --- IdentifierExpr resolution: parameters and locals (Phase 3B) ---

void testParameterUseResolvesToParameterSymbol() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: i32) {\n    let y = x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(declY.initializer().kind() == ExprKind::Identifier);
    const auto& xUse = static_cast<const IdentifierExpr&>(declY.initializer());

    const auto resolved = result.model.resolution(xUse);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testEarlierLocalUseResolvesToFirstLocal() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    let y = x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(xId.has_value());

    KAI_CHECK(declY.initializer().kind() == ExprKind::Identifier);
    const auto& xUse = static_cast<const IdentifierExpr&>(declY.initializer());
    const auto resolved = result.model.resolution(xUse);
    KAI_CHECK(resolved.has_value());
    if (xId && resolved) {
        KAI_CHECK(*resolved == *xId);
    }
}

void testLaterLocalIsNotVisibleToEarlierUse() {
    // fn f() { let y = x  let x = 1 } - locals are not hoisted.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let y = x\n    let x = 1\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        const auto& error = result.model.errors()[0];
        KAI_CHECK(error.kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(error.primarySpan) == "x");
        KAI_CHECK(!error.relatedSpan.has_value());
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declY = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& xUse = static_cast<const IdentifierExpr&>(declY.initializer());
    KAI_CHECK(!result.model.resolution(xUse).has_value());
}

void testSelfInitializerProducesUnknownIdentifier() {
    // fn f() { let x = x }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = x\n}");

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
    const auto& rhsX = static_cast<const IdentifierExpr&>(declX.initializer());
    KAI_CHECK(!result.model.resolution(rhsX).has_value());

    // The declaration x itself still gets a Local SymbolId afterward.
    KAI_CHECK(result.model.declarationSymbol(declX.name()).has_value());
}

void testNestedShadowInitializerResolvesOuter() {
    // fn f() { let x = 1  if true { let x = x } }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n\n    if true {\n        let x = x\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto outerId = result.model.declarationSymbol(outerX.name());
    KAI_CHECK(outerId.has_value());

    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[1]);
    const auto& innerX = static_cast<const VarDeclStmt&>(*ifStmt.branches()[0].body->statements()[0]);
    const auto& innerRhs = static_cast<const IdentifierExpr&>(innerX.initializer());

    const auto resolved = result.model.resolution(innerRhs);
    KAI_CHECK(resolved.has_value());
    if (outerId && resolved) {
        KAI_CHECK(*resolved == *outerId);
    }

    const auto innerId = result.model.declarationSymbol(innerX.name());
    KAI_CHECK(innerId.has_value());
    if (outerId && innerId) {
        KAI_CHECK(!(*outerId == *innerId));
    }
}

void testParameterBodyDuplicateInitializerResolvesParameter() {
    // fn f(x: i32) { let x = x } - the RHS is analyzed BEFORE the
    // duplicate local is declared, so it resolves to the parameter.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: i32) {\n    let x = x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::DuplicateSymbol);
    }

    const auto& localX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& rhsX = static_cast<const IdentifierExpr&>(localX.initializer());

    const auto resolved = result.model.resolution(rhsX);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }

    const auto localId = result.model.declarationSymbol(localX.name());
    KAI_CHECK(localId.has_value());
    if (paramId && localId) {
        KAI_CHECK(!(*paramId == *localId));
    }
}

// --- Top-level function resolution ---

void testForwardFunctionReferenceResolves() {
    // fn main() { helper() }  fn helper() {}
    SourceManager sm;
    Analyzed result = analyze(sm, "fn main() {\n    helper()\n}\nfn helper() {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& helperFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto helperId = result.model.declarationSymbol(helperFn.name());
    KAI_CHECK(helperId.has_value());

    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

    const auto resolved = result.model.resolution(callee);
    KAI_CHECK(resolved.has_value());
    if (helperId && resolved) {
        KAI_CHECK(*resolved == *helperId);
    }
}

void testSelfRecursionResolvesToOwnFunctionSymbol() {
    // fn fib(n: i32) -> i32 { return fib(n) }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn fib(n: i32) -> i32 {\n    return fib(n)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto fnId = result.model.declarationSymbol(fn.name());
    KAI_CHECK(fnId.has_value());

    const auto& returnStmt = static_cast<const ReturnStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(returnStmt.value() != nullptr);
    const auto& call = static_cast<const CallExpr&>(*returnStmt.value());
    const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

    const auto resolved = result.model.resolution(callee);
    KAI_CHECK(resolved.has_value());
    if (fnId && resolved) {
        KAI_CHECK(*resolved == *fnId);
    }
}

void testMutualFunctionReferencesResolve() {
    // fn a() { b() }  fn b() { a() }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn a() {\n    b()\n}\nfn b() {\n    a()\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fnA = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& fnB = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto aId = result.model.declarationSymbol(fnA.name());
    const auto bId = result.model.declarationSymbol(fnB.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());

    const auto& callInA = static_cast<const CallExpr&>(static_cast<const ExprStmt&>(*fnA.body().statements()[0]).expr());
    const auto& calleeInA = static_cast<const IdentifierExpr&>(callInA.callee());
    const auto resolvedInA = result.model.resolution(calleeInA);
    KAI_CHECK(resolvedInA.has_value());
    if (bId && resolvedInA) {
        KAI_CHECK(*resolvedInA == *bId);
    }

    const auto& callInB = static_cast<const CallExpr&>(static_cast<const ExprStmt&>(*fnB.body().statements()[0]).expr());
    const auto& calleeInB = static_cast<const IdentifierExpr&>(callInB.callee());
    const auto resolvedInB = result.model.resolution(calleeInB);
    KAI_CHECK(resolvedInB.has_value());
    if (aId && resolvedInB) {
        KAI_CHECK(*resolvedInB == *aId);
    }
}

void testDuplicateLocalFinalUseResolvesFirstDeclaration() {
    // fn f() { let x = 1  let x = 2  x }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    let x = 2\n    x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::DuplicateSymbol);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& first = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto firstId = result.model.declarationSymbol(first.name());
    KAI_CHECK(firstId.has_value());

    const auto& finalUseStmt = static_cast<const ExprStmt&>(*fn.body().statements()[2]);
    KAI_CHECK(finalUseStmt.expr().kind() == ExprKind::Identifier);
    const auto& finalUse = static_cast<const IdentifierExpr&>(finalUseStmt.expr());

    const auto resolved = result.model.resolution(finalUse);
    KAI_CHECK(resolved.has_value());
    if (firstId && resolved) {
        KAI_CHECK(*resolved == *firstId);
    }
}

void testDuplicateTopLevelFunctionUseResolvesFirstDeclaration() {
    // fn a() {}  fn a() {}  fn main() { a() }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn a() {}\nfn a() {}\nfn main() {\n    a()\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::DuplicateSymbol);
    }

    const auto& firstA = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto firstId = result.model.declarationSymbol(firstA.name());
    KAI_CHECK(firstId.has_value());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[2]);
    const auto& callStmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(callStmt.expr());
    const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

    const auto resolved = result.model.resolution(callee);
    KAI_CHECK(resolved.has_value());
    if (firstId && resolved) {
        KAI_CHECK(*resolved == *firstId);
    }
}

// --- Condition/iterable scope placement ---

void testIfConditionResolvesInEnclosingScope() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(cond: bool) {\n    if cond {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(ifStmt.branches()[0].condition->kind() == ExprKind::Identifier);
    const auto& condition = static_cast<const IdentifierExpr&>(*ifStmt.branches()[0].condition);

    const auto resolved = result.model.resolution(condition);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testWhileConditionResolvesInEnclosingScope() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(cond: bool) {\n    while cond {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& whileStmt = static_cast<const WhileStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(whileStmt.condition().kind() == ExprKind::Identifier);
    const auto& condition = static_cast<const IdentifierExpr&>(whileStmt.condition());

    const auto resolved = result.model.resolution(condition);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testForIterableResolvesOuterNotLoopVariable() {
    // fn f() { let x = 1  for x in x { } } - the iterable is analyzed
    // before the loop variable exists in any scope.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    for x in x {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto outerId = result.model.declarationSymbol(outerX.name());
    KAI_CHECK(outerId.has_value());

    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);
    KAI_CHECK(forStmt.iterable().kind() == ExprKind::Identifier);
    const auto& iterable = static_cast<const IdentifierExpr&>(forStmt.iterable());

    const auto resolved = result.model.resolution(iterable);
    KAI_CHECK(resolved.has_value());
    if (outerId && resolved) {
        KAI_CHECK(*resolved == *outerId);
    }

    const auto loopVarId = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(loopVarId.has_value());
    if (outerId && loopVarId) {
        KAI_CHECK(!(*outerId == *loopVarId));
    }
}

void testForIterableWithoutOuterProducesUnknownIdentifier() {
    // fn f() { for x in x { } } with no outer x.
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    for x in x {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
    const auto& iterable = static_cast<const IdentifierExpr&>(forStmt.iterable());
    KAI_CHECK(!result.model.resolution(iterable).has_value());
}

void testForBodyUseResolvesToLoopVariable() {
    SourceManager sm;
    Analyzed result =
        analyze(sm, "fn f() {\n    let values = 1\n    for item in values {\n        print(item)\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);
    const auto loopVarId = result.model.declarationSymbol(forStmt.variable());
    KAI_CHECK(loopVarId.has_value());

    const auto& printStmt = static_cast<const ExprStmt&>(*forStmt.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(printStmt.expr());
    const auto& itemArg = static_cast<const IdentifierExpr&>(*call.arguments()[0]);

    const auto resolved = result.model.resolution(itemArg);
    KAI_CHECK(resolved.has_value());
    if (loopVarId && resolved) {
        KAI_CHECK(*resolved == *loopVarId);
    }
}

// --- Nested/sibling scope resolution (innermost wins, no leakage) ---

void testNestedShadowingInnermostWinsAtEachUse() {
    // fn f() { let x = 1  if true { let x = 2  print(x) }  print(x) }
    SourceManager sm;
    Analyzed result = analyze(
        sm, "fn f() {\n    let x = 1\n\n    if true {\n        let x = 2\n        print(x)\n    }\n\n    print(x)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto outerId = result.model.declarationSymbol(outerX.name());
    KAI_CHECK(outerId.has_value());

    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[1]);
    const auto& innerX = static_cast<const VarDeclStmt&>(*ifStmt.branches()[0].body->statements()[0]);
    const auto innerId = result.model.declarationSymbol(innerX.name());
    KAI_CHECK(innerId.has_value());

    const auto& innerPrintStmt = static_cast<const ExprStmt&>(*ifStmt.branches()[0].body->statements()[1]);
    const auto& innerCall = static_cast<const CallExpr&>(innerPrintStmt.expr());
    const auto& innerArg = static_cast<const IdentifierExpr&>(*innerCall.arguments()[0]);

    const auto& outerPrintStmt = static_cast<const ExprStmt&>(*fn.body().statements()[2]);
    const auto& outerCall = static_cast<const CallExpr&>(outerPrintStmt.expr());
    const auto& outerArg = static_cast<const IdentifierExpr&>(*outerCall.arguments()[0]);

    const auto innerResolved = result.model.resolution(innerArg);
    const auto outerResolved = result.model.resolution(outerArg);
    KAI_CHECK(innerResolved.has_value());
    KAI_CHECK(outerResolved.has_value());
    if (innerId && innerResolved) {
        KAI_CHECK(*innerResolved == *innerId);
    }
    if (outerId && outerResolved) {
        KAI_CHECK(*outerResolved == *outerId);
    }
}

void testSiblingScopeDoesNotLeakDeclarations() {
    // fn f() { if true { let x = 1 } else { x } }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    if true {\n        let x = 1\n    } else {\n        x\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& ifStmt = static_cast<const IfStmt&>(*fn.body().statements()[0]);
    const auto& elseStmt = static_cast<const ExprStmt&>(*ifStmt.elseClause()->body->statements()[0]);
    KAI_CHECK(elseStmt.expr().kind() == ExprKind::Identifier);
    const auto& elseX = static_cast<const IdentifierExpr&>(elseStmt.expr());

    KAI_CHECK(!result.model.resolution(elseX).has_value());
}

// --- Expression traversal shapes (names only, no typing) ---

void testCallExprTraversesCalleeAndArguments() {
    // fn f(a: i32) { unknown_fn(a) }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    unknown_fn(a)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "unknown_fn");
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(stmt.expr());
    const auto& argA = static_cast<const IdentifierExpr&>(*call.arguments()[0]);
    const auto resolved = result.model.resolution(argA);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testParenExprTraversesInner() {
    // fn f(a: i32) { (a) }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    (a)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Paren);
    const auto& paren = static_cast<const ParenExpr&>(stmt.expr());
    const auto& inner = static_cast<const IdentifierExpr&>(paren.inner());

    const auto resolved = result.model.resolution(inner);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testUnaryExprTraversesOperand() {
    // fn f(a: i32) { -a }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    -a\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Unary);
    const auto& unary = static_cast<const UnaryExpr&>(stmt.expr());
    const auto& operand = static_cast<const IdentifierExpr&>(unary.operand());

    const auto resolved = result.model.resolution(operand);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

void testBinaryExprTraversesLeftAndRight() {
    // fn f(a: i32, b: i32) { a + b }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32, b: i32) {\n    a + b\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto aId = result.model.declarationSymbol(fn.params()[0].name);
    const auto bId = result.model.declarationSymbol(fn.params()[1].name);
    KAI_CHECK(aId.has_value());
    KAI_CHECK(bId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Binary);
    const auto& binary = static_cast<const BinaryExpr&>(stmt.expr());
    const auto& left = static_cast<const IdentifierExpr&>(binary.left());
    const auto& right = static_cast<const IdentifierExpr&>(binary.right());

    const auto leftResolved = result.model.resolution(left);
    const auto rightResolved = result.model.resolution(right);
    KAI_CHECK(leftResolved.has_value());
    KAI_CHECK(rightResolved.has_value());
    if (aId && leftResolved) {
        KAI_CHECK(*leftResolved == *aId);
    }
    if (bId && rightResolved) {
        KAI_CHECK(*rightResolved == *bId);
    }
}

void testAssignmentExprTraversesTargetAndValue() {
    // fn f(a: i32) { mut x = 1  x = a }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    mut x = 1\n    x = a\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto aId = result.model.declarationSymbol(fn.params()[0].name);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto xId = result.model.declarationSymbol(declX.name());
    KAI_CHECK(aId.has_value());
    KAI_CHECK(xId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Assignment);
    const auto& assignment = static_cast<const AssignmentExpr&>(stmt.expr());
    const auto& target = static_cast<const IdentifierExpr&>(assignment.target());
    const auto& value = static_cast<const IdentifierExpr&>(assignment.value());

    const auto targetResolved = result.model.resolution(target);
    const auto valueResolved = result.model.resolution(value);
    KAI_CHECK(targetResolved.has_value());
    KAI_CHECK(valueResolved.has_value());
    if (xId && targetResolved) {
        KAI_CHECK(*targetResolved == *xId);
    }
    if (aId && valueResolved) {
        KAI_CHECK(*valueResolved == *aId);
    }
}

void testArrayLiteralExprTraversesEveryElement() {
    // fn f(a: i32) { [a, unknown_elem] }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    [a, unknown_elem]\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "unknown_elem");
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& array = static_cast<const ArrayLiteralExpr&>(stmt.expr());
    const auto& first = static_cast<const IdentifierExpr&>(*array.elements()[0]);
    const auto& second = static_cast<const IdentifierExpr&>(*array.elements()[1]);

    const auto firstResolved = result.model.resolution(first);
    KAI_CHECK(firstResolved.has_value());
    if (paramId && firstResolved) {
        KAI_CHECK(*firstResolved == *paramId);
    }
    KAI_CHECK(!result.model.resolution(second).has_value());
}

void testIndexExprTraversesObjectAndIndex() {
    // fn f(arr: i32, i: i32) { arr[i] }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(arr: i32, i: i32) {\n    arr[i]\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto arrId = result.model.declarationSymbol(fn.params()[0].name);
    const auto iId = result.model.declarationSymbol(fn.params()[1].name);
    KAI_CHECK(arrId.has_value());
    KAI_CHECK(iId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Index);
    const auto& index = static_cast<const IndexExpr&>(stmt.expr());
    const auto& object = static_cast<const IdentifierExpr&>(index.object());
    const auto& indexExpr = static_cast<const IdentifierExpr&>(index.index());

    const auto objectResolved = result.model.resolution(object);
    const auto indexResolved = result.model.resolution(indexExpr);
    KAI_CHECK(objectResolved.has_value());
    KAI_CHECK(indexResolved.has_value());
    if (arrId && objectResolved) {
        KAI_CHECK(*objectResolved == *arrId);
    }
    if (iId && indexResolved) {
        KAI_CHECK(*indexResolved == *iId);
    }
}

void testErrorPropagationExprTraversesOperand() {
    // fn f(a: i32) { a? }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(a: i32) {\n    a?\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto paramId = result.model.declarationSymbol(fn.params()[0].name);
    KAI_CHECK(paramId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::ErrorPropagation);
    const auto& propagation = static_cast<const ErrorPropagationExpr&>(stmt.expr());
    const auto& operand = static_cast<const IdentifierExpr&>(propagation.operand());

    const auto resolved = result.model.resolution(operand);
    KAI_CHECK(resolved.has_value());
    if (paramId && resolved) {
        KAI_CHECK(*resolved == *paramId);
    }
}

// --- MemberExpr: object is lexical, member is not ---

void testMemberExprResolvesObjectNotMemberName() {
    // fn f() { let obj = 1  obj.unknown_member }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let obj = 1\n    obj.unknown_member\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declObj = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto objId = result.model.declarationSymbol(declObj.name());
    KAI_CHECK(objId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Member);
    const auto& member = static_cast<const MemberExpr&>(stmt.expr());
    const auto& object = static_cast<const IdentifierExpr&>(member.object());

    const auto resolved = result.model.resolution(object);
    KAI_CHECK(resolved.has_value());
    if (objId && resolved) {
        KAI_CHECK(*resolved == *objId);
    }
}

void testMemberExprUnknownObjectProducesOnlyOneError() {
    // fn f() { unknown_obj.member }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    unknown_obj.member\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "unknown_obj");
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    const auto& member = static_cast<const MemberExpr&>(stmt.expr());
    const auto& object = static_cast<const IdentifierExpr&>(member.object());
    KAI_CHECK(!result.model.resolution(object).has_value());
}

// --- Prelude (print/panic/assert) ---

void testPreludeBuiltinsResolveWithoutError() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn main() {\n    print(1)\n    panic()\n    assert(true)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const char* names[] = {"print", "panic", "assert"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[i]);
        const auto& call = static_cast<const CallExpr&>(stmt.expr());
        const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

        const auto resolved = result.model.resolution(callee);
        KAI_CHECK(resolved.has_value());
        if (!resolved) {
            continue;
        }

        const Symbol& symbol = result.model.symbol(*resolved);
        KAI_CHECK(symbol.kind == SymbolKind::Builtin);
        KAI_CHECK(symbol.name == names[i]);
        KAI_CHECK(!symbol.declaredAt.has_value());
        KAI_CHECK(!symbol.signature.has_value());
    }
}

void testUserFunctionShadowsPreludeName() {
    // fn print() {}  fn main() { print() }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn print() {}\nfn main() {\n    print()\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& userPrint = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto userPrintId = result.model.declarationSymbol(userPrint.name());
    KAI_CHECK(userPrintId.has_value());

    const auto& mainFn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[1]);
    const auto& stmt = static_cast<const ExprStmt&>(*mainFn.body().statements()[0]);
    const auto& call = static_cast<const CallExpr&>(stmt.expr());
    const auto& callee = static_cast<const IdentifierExpr&>(call.callee());

    const auto resolved = result.model.resolution(callee);
    KAI_CHECK(resolved.has_value());
    if (userPrintId && resolved) {
        KAI_CHECK(*resolved == *userPrintId);
    }
    if (resolved) {
        KAI_CHECK(result.model.symbol(*resolved).kind == SymbolKind::Function);
    }
}

void testLocalShadowsPreludeName() {
    // fn main() { let print = 1  print }
    SourceManager sm;
    Analyzed result = analyze(sm, "fn main() {\n    let print = 1\n    print\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declPrint = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto localId = result.model.declarationSymbol(declPrint.name());
    KAI_CHECK(localId.has_value());

    const auto& stmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    KAI_CHECK(stmt.expr().kind() == ExprKind::Identifier);
    const auto& use = static_cast<const IdentifierExpr&>(stmt.expr());

    const auto resolved = result.model.resolution(use);
    KAI_CHECK(resolved.has_value());
    if (localId && resolved) {
        KAI_CHECK(*resolved == *localId);
    }
    if (resolved) {
        KAI_CHECK(result.model.symbol(*resolved).kind == SymbolKind::Local);
    }
}

// --- Multiple UnknownIdentifier errors ---

void testMultipleUnknownIdentifiersAreCollectedInSourceOrder() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    first_unknown\n    second_unknown\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 2);
    if (result.model.errors().size() == 2) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(result.model.errors()[0].primarySpan) == "first_unknown");
        KAI_CHECK(result.model.errors()[1].kind == SemanticErrorKind::UnknownIdentifier);
        KAI_CHECK(sm.text(result.model.errors()[1].primarySpan) == "second_unknown");
    }
}

// --- SymbolId stability across vector growth ---

void testSymbolIdsRemainStableAfterVectorGrowth() {
    std::string source;
    for (int i = 0; i < 20; ++i) {
        source += "fn f" + std::to_string(i) + "() {}\n";
    }

    SourceManager sm;
    Analyzed result = analyze(sm, source);

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& first = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto firstId = result.model.declarationSymbol(first.name());
    KAI_CHECK(firstId.has_value());

    // By the time analyze() returns, 19 more addSymbol() calls have
    // happened after `first`'s, almost certainly reallocating the
    // backing vector at least once. firstId must still resolve to the
    // correct Symbol regardless.
    if (firstId) {
        KAI_CHECK(result.model.symbol(*firstId).name == "f0");
    }

    const auto& last = static_cast<const FunctionDecl&>(*result.parsed->declarations()[19]);
    const auto lastId = result.model.declarationSymbol(last.name());
    KAI_CHECK(lastId.has_value());
    if (lastId) {
        KAI_CHECK(result.model.symbol(*lastId).name == "f19");
    }
}

// --- Analyzer instance reused across independent files: no leakage ---

void testAnalyzerInstanceDoesNotLeakStateBetweenFiles() {
    SourceManager sm;
    const FileId fileA = sm.addVirtualFile("a.kai", "fn a() {\n    print(1)\n}\nfn shared() {}");
    const FileId fileB = sm.addVirtualFile("b.kai", "fn b(x: Foo) {\n    print(2)\n}\nfn shared() {}");

    Parser parserA(sm, fileA);
    auto parsedA = parserA.parseSourceFile();
    Parser parserB(sm, fileB);
    auto parsedB = parserB.parseSourceFile();

    KAI_CHECK(parsedA.has_value());
    KAI_CHECK(parsedB.has_value());
    if (!parsedA || !parsedB) {
        return;
    }

    SemanticAnalyzer analyzer(sm);
    const SemanticModel modelA = analyzer.analyze(*parsedA);
    const SemanticModel modelB = analyzer.analyze(*parsedB);

    // fileA declares `shared` once - no duplicate there.
    KAI_CHECK(modelA.errors().empty());

    // fileB also declares `shared` once (on its own) plus one genuinely
    // unknown parameter type. If the analyzer's internal top-level name
    // table leaked from the first analyze() call into the second,
    // fileB's `shared` would incorrectly report as a DuplicateSymbol
    // against fileA's `shared` - it must not.
    KAI_CHECK(modelB.errors().size() == 1);
    if (modelB.errors().size() == 1) {
        KAI_CHECK(modelB.errors()[0].kind == SemanticErrorKind::UnknownType);
    }

    const auto& sharedInA = static_cast<const FunctionDecl&>(*parsedA->declarations()[1]);
    const auto& sharedInB = static_cast<const FunctionDecl&>(*parsedB->declarations()[1]);
    KAI_CHECK(modelA.declarationSymbol(sharedInA.name()).has_value());
    KAI_CHECK(modelB.declarationSymbol(sharedInB.name()).has_value());

    // Each call gets its own independent Builtin prelude: `print` in
    // fileA and `print` in fileB each resolve cleanly, in their own
    // model, to a Builtin Symbol - not shared, duplicated, or missing
    // because the other call already "used it up".
    const auto& fnA = static_cast<const FunctionDecl&>(*parsedA->declarations()[0]);
    const auto& callStmtA = static_cast<const ExprStmt&>(*fnA.body().statements()[0]);
    const auto& calleeA = static_cast<const IdentifierExpr&>(static_cast<const CallExpr&>(callStmtA.expr()).callee());
    const auto resolvedA = modelA.resolution(calleeA);
    KAI_CHECK(resolvedA.has_value());
    if (resolvedA) {
        KAI_CHECK(modelA.symbol(*resolvedA).kind == SymbolKind::Builtin);
        KAI_CHECK(modelA.symbol(*resolvedA).name == "print");
    }

    const auto& fnB = static_cast<const FunctionDecl&>(*parsedB->declarations()[0]);
    const auto& callStmtB = static_cast<const ExprStmt&>(*fnB.body().statements()[0]);
    const auto& calleeB = static_cast<const IdentifierExpr&>(static_cast<const CallExpr&>(callStmtB.expr()).callee());
    const auto resolvedB = modelB.resolution(calleeB);
    KAI_CHECK(resolvedB.has_value());
    if (resolvedB) {
        KAI_CHECK(modelB.symbol(*resolvedB).kind == SymbolKind::Builtin);
        KAI_CHECK(modelB.symbol(*resolvedB).name == "print");
    }
}

} // namespace

int main() {
    testPrimitiveParameterAndReturnSignature();
    testMissingAndExplicitUnitReturnBothResolveToUnit();
    testSourceFunctionSymbolHasSourceNameAndDeclaredAtSpan();
    testUnknownParameterTypeProducesOneError();
    testDeferredTypeShapesResolveToUnresolvedWithNoErrors();
    testDuplicateTopLevelFunctionProducesExactlyOneError();
    testMultipleErrorsAreCollectedInDeterministicSourceOrder();
    testUnresolvedBodyIdentifierProducesUnknownIdentifier();

    testParameterSymbolsHaveCorrectFields();
    testDuplicateParametersProduceOneErrorAndTwoSymbols();

    testLocalSymbolsHaveCorrectFieldsAndMutability();
    testDuplicateLocalsProduceOneErrorAndTwoSymbols();
    testParameterAndBodyLocalCollisionIsDuplicateSymbol();

    testNestedIfBlockShadowsParameter();
    testSiblingIfElseBranchesDoNotCollide();
    testWhileBodyShadowsOuterLocal();

    testForVariableSymbol();
    testForBodyCollisionWithLoopVariable();
    testForVariableShadowsOuterLocal();

    testLocalUnknownAnnotationProducesUnknownTypeNotUnknownIdentifier();

    testParameterUseResolvesToParameterSymbol();
    testEarlierLocalUseResolvesToFirstLocal();
    testLaterLocalIsNotVisibleToEarlierUse();
    testSelfInitializerProducesUnknownIdentifier();
    testNestedShadowInitializerResolvesOuter();
    testParameterBodyDuplicateInitializerResolvesParameter();

    testForwardFunctionReferenceResolves();
    testSelfRecursionResolvesToOwnFunctionSymbol();
    testMutualFunctionReferencesResolve();
    testDuplicateLocalFinalUseResolvesFirstDeclaration();
    testDuplicateTopLevelFunctionUseResolvesFirstDeclaration();

    testIfConditionResolvesInEnclosingScope();
    testWhileConditionResolvesInEnclosingScope();
    testForIterableResolvesOuterNotLoopVariable();
    testForIterableWithoutOuterProducesUnknownIdentifier();
    testForBodyUseResolvesToLoopVariable();

    testNestedShadowingInnermostWinsAtEachUse();
    testSiblingScopeDoesNotLeakDeclarations();

    testCallExprTraversesCalleeAndArguments();
    testParenExprTraversesInner();
    testUnaryExprTraversesOperand();
    testBinaryExprTraversesLeftAndRight();
    testAssignmentExprTraversesTargetAndValue();
    testArrayLiteralExprTraversesEveryElement();
    testIndexExprTraversesObjectAndIndex();
    testErrorPropagationExprTraversesOperand();

    testMemberExprResolvesObjectNotMemberName();
    testMemberExprUnknownObjectProducesOnlyOneError();

    testPreludeBuiltinsResolveWithoutError();
    testUserFunctionShadowsPreludeName();
    testLocalShadowsPreludeName();

    testMultipleUnknownIdentifiersAreCollectedInSourceOrder();

    testSymbolIdsRemainStableAfterVectorGrowth();
    testAnalyzerInstanceDoesNotLeakStateBetweenFiles();

    return kai::test::failureCount == 0 ? 0 : 1;
}
