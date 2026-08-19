#include "kai/ast/Node.hpp"
#include "kai/ast/Expr.hpp" // LiteralExpr: concrete NodeBase<ExprKind> subject for these checks
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::Identifier;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;

namespace {

void testIdentifierIsSpanOnly() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "value");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 5));

    const Identifier id{span};

    KAI_CHECK(id.span == span);
    KAI_CHECK(sm.text(id.span) == "value");
}

// LiteralExpr stands in here for "every NodeBase<Kind>-derived node
// stores and exposes its own kind()/span() directly" - the invariant
// NodeBase itself is meant to guarantee, regardless of which concrete
// leaf class is used to observe it.
void testNodeBaseKindAndSpanAreStoredExplicitly() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "42");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 2));

    const LiteralExpr literal(LiteralKind::Integer, span);

    KAI_CHECK(literal.kind() == kai::ast::ExprKind::Literal);
    KAI_CHECK(literal.span() == span);
}

} // namespace

int main() {
    testIdentifierIsSpanOnly();
    testNodeBaseKindAndSpanAreStoredExplicitly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
