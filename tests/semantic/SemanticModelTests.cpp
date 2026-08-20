#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Symbol.hpp"

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::semantic::SemanticModel;
using kai::semantic::SymbolId;

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

} // namespace

int main() {
    testSymbolIdDefaultIsInvalid();
    testDefaultSymbolIdsCompareEqual();

    testFreshModelHasNoErrors();
    testFreshModelResolvesNoIdentifierUse();
    testFreshModelResolvesNoDeclaration();

    return kai::test::failureCount == 0 ? 0 : 1;
}
