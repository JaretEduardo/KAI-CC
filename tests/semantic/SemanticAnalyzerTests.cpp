#include "kai/semantic/SemanticAnalyzer.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::CallExpr;
using kai::ast::ExprKind;
using kai::ast::ExprStmt;
using kai::ast::ForStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
using kai::ast::IfStmt;
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

// --- Body is ignored in this phase ---

void testFunctionBodyIsNotAnalyzed() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    unknown_name\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    // No UnknownIdentifier - or any other error - comes from the body.
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& exprStmt = static_cast<const ExprStmt&>(*fn.body().statements()[0]);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Identifier);
    const auto& identifierExpr = static_cast<const IdentifierExpr&>(exprStmt.expr());

    KAI_CHECK(!result.model.resolution(identifierExpr).has_value());
}

// --- No parameter symbols yet ---

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
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    for item in values {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // The iterable ("values") is not analyzed in this phase - no error
    // comes from it being an unresolved name.
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);

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
    Analyzed result = analyze(sm, "fn f() {\n    for item in values {\n        let item = 1\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[0]);
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
    Analyzed result = analyze(sm, "fn f() {\n    let item = 1\n    for item in values {\n    }\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& outerItem = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& forStmt = static_cast<const ForStmt&>(*fn.body().statements()[1]);

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
    Analyzed result = analyze(sm, "fn f() {\n    let x: Foo = value\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declX = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);

    // Exactly one error, and it is UnknownType - the initializer
    // "value" is never inspected in this phase, so it cannot produce an
    // UnknownIdentifier (that error kind does not even get emitted yet).
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

// --- Body identifiers remain unresolved (Phase 3A boundary) ---

void testBodyIdentifiersRemainUnresolvedInThisPhase() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let x = 1\n    print(x)\n    unknown_name\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);

    const auto& printCallStmt = static_cast<const ExprStmt&>(*fn.body().statements()[1]);
    const auto& call = static_cast<const CallExpr&>(printCallStmt.expr());
    KAI_CHECK(call.callee().kind() == ExprKind::Identifier);
    const auto& printCallee = static_cast<const IdentifierExpr&>(call.callee());
    KAI_CHECK(call.arguments()[0]->kind() == ExprKind::Identifier);
    const auto& xArgument = static_cast<const IdentifierExpr&>(*call.arguments()[0]);

    const auto& unknownStmt = static_cast<const ExprStmt&>(*fn.body().statements()[2]);
    KAI_CHECK(unknownStmt.expr().kind() == ExprKind::Identifier);
    const auto& unknownName = static_cast<const IdentifierExpr&>(unknownStmt.expr());

    KAI_CHECK(!result.model.resolution(printCallee).has_value());
    KAI_CHECK(!result.model.resolution(xArgument).has_value());
    KAI_CHECK(!result.model.resolution(unknownName).has_value());
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
    const FileId fileA = sm.addVirtualFile("a.kai", "fn a() {}\nfn shared() {}");
    const FileId fileB = sm.addVirtualFile("b.kai", "fn b(x: Foo) {}\nfn shared() {}");

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
    testFunctionBodyIsNotAnalyzed();

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
    testBodyIdentifiersRemainUnresolvedInThisPhase();

    testSymbolIdsRemainStableAfterVectorGrowth();
    testAnalyzerInstanceDoesNotLeakStateBetweenFiles();

    return kai::test::failureCount == 0 ? 0 : 1;
}
