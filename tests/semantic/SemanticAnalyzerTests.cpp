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
using kai::ast::ExprKind;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
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

void testParameterHasNoDeclarationSymbolYet() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(x: i32) {}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);

    KAI_CHECK(result.model.declarationSymbol(fn.name()).has_value());
    KAI_CHECK(!result.model.declarationSymbol(fn.params()[0].name).has_value());
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
        KAI_CHECK(sm.text(result.model.symbol(*firstId).name.span) == "f0");
    }

    const auto& last = static_cast<const FunctionDecl&>(*result.parsed->declarations()[19]);
    const auto lastId = result.model.declarationSymbol(last.name());
    KAI_CHECK(lastId.has_value());
    if (lastId) {
        KAI_CHECK(sm.text(result.model.symbol(*lastId).name.span) == "f19");
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
    testUnknownParameterTypeProducesOneError();
    testDeferredTypeShapesResolveToUnresolvedWithNoErrors();
    testDuplicateTopLevelFunctionProducesExactlyOneError();
    testMultipleErrorsAreCollectedInDeterministicSourceOrder();
    testFunctionBodyIsNotAnalyzed();
    testParameterHasNoDeclarationSymbolYet();
    testSymbolIdsRemainStableAfterVectorGrowth();
    testAnalyzerInstanceDoesNotLeakStateBetweenFiles();

    return kai::test::failureCount == 0 ? 0 : 1;
}
