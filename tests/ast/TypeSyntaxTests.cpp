#include "kai/ast/TypeSyntax.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::Identifier;
using kai::ast::NamedTypeSyntax;
using kai::ast::TypeSyntaxKind;

namespace {

void testNamedTypeSyntaxConstruction() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "i32");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 3));

    const NamedTypeSyntax type(Identifier{span}, span);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Named);
    KAI_CHECK(type.span() == span);
    KAI_CHECK(type.name().span == span);
    KAI_CHECK(sm.text(type.name().span) == "i32");
}

// Primitive names and user-defined names are syntactically
// indistinguishable: both produce a NamedTypeSyntax. Deciding which is
// which is semantic analysis's job, not the parser's.
void testNamedTypeSyntaxDoesNotDistinguishPrimitiveFromUserNames() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "User");
    const SourceSpan span(SourceLocation(file, 0), SourceLocation(file, 4));

    const NamedTypeSyntax type(Identifier{span}, span);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Named);
}

} // namespace

int main() {
    testNamedTypeSyntaxConstruction();
    testNamedTypeSyntaxDoesNotDistinguishPrimitiveFromUserNames();

    return kai::test::failureCount == 0 ? 0 : 1;
}
