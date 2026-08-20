#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Symbol.hpp"

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <optional>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::semantic::SemanticModel;
using kai::semantic::Symbol;
using kai::semantic::SymbolId;
using kai::semantic::SymbolKind;
using kai::semantic::Type;

namespace {

// --- SymbolId: default validity and value semantics through the public
// API ---
//
// SymbolId's only public constructor is the default one; a *valid*
// SymbolId can only be produced by SemanticModel::addSymbol(), which is
// private and friended exclusively to the not-yet-implemented
// SemanticAnalyzer (see SemanticModel.hpp). These tests cover exactly
// what is genuinely exercisable through the public API in this phase -
// the same "invalid by default" shape kai::FileId exposes before a
// SourceManager produces a real one (see SourceLocationTests.cpp).

void testSymbolIdDefaultIsInvalid() {
    const SymbolId id;
    KAI_CHECK(!id.isValid());
}

void testDefaultSymbolIdsCompareEqual() {
    KAI_CHECK(SymbolId() == SymbolId());
}

// --- SemanticModel: public query behavior on a freshly constructed
// model ---
//
// No SemanticAnalyzer exists yet to populate a SemanticModel, so these
// tests exercise exactly the "nothing has been recorded" path of the
// public query API - a real, meaningful behavior every
// resolution()/declarationSymbol() call must handle correctly, not a
// placeholder. Deliberately no test-only mutation API is added merely to
// exercise the populated path early; that belongs to
// SemanticAnalyzerTests once SemanticAnalyzer exists.

void testFreshModelHasNoErrors() {
    const SemanticModel model;
    KAI_CHECK(model.errors().empty());
}

void testFreshModelResolvesNoIdentifierUse() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "value");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 5));
    const IdentifierExpr expr(Identifier{span}, span);

    const SemanticModel model;
    KAI_CHECK(!model.resolution(expr).has_value());
}

void testFreshModelResolvesNoDeclaration() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "value");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 5));
    const Identifier identifier{span};

    const SemanticModel model;
    KAI_CHECK(!model.declarationSymbol(identifier).has_value());
}

// --- Symbol: can represent a source-less semantic entity ---
//
// Foundation/data-model coverage only, per the Phase 3 prelude-readiness
// refactor: proves Symbol itself - a plain, publicly constructible
// aggregate, no SemanticModel/SemanticAnalyzer involvement needed - can
// describe a compiler-defined entry (print/panic/assert, eventually)
// with no AST Identifier and no real source declaration span, without
// fabricating a SourceSpan or any AST data to stand in for one. Builtin
// *creation* by SemanticAnalyzer is explicitly not implemented yet; this
// only proves the representation is ready for it.

void testSymbolCanRepresentASourcelessBuiltinEntity() {
    const Symbol builtin{
        SymbolKind::Builtin, "print", std::nullopt, false, Type::unresolved(), std::nullopt,
    };

    KAI_CHECK(builtin.kind == SymbolKind::Builtin);
    KAI_CHECK(builtin.name == "print");
    KAI_CHECK(!builtin.declaredAt.has_value());
    KAI_CHECK(!builtin.signature.has_value());
}

} // namespace

int main() {
    testSymbolIdDefaultIsInvalid();
    testDefaultSymbolIdsCompareEqual();

    testFreshModelHasNoErrors();
    testFreshModelResolvesNoIdentifierUse();
    testFreshModelResolvesNoDeclaration();

    testSymbolCanRepresentASourcelessBuiltinEntity();

    return kai::test::failureCount == 0 ? 0 : 1;
}
