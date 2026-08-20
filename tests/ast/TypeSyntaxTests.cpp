#include "kai/ast/TypeSyntax.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <memory>
#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::ArrayTypeSyntax;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::GenericTypeSyntax;
using kai::ast::Identifier;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::NamedTypeSyntax;
using kai::ast::ReferenceMutability;
using kai::ast::ReferenceTypeSyntax;
using kai::ast::SliceTypeSyntax;
using kai::ast::TypeSyntaxKind;
using kai::ast::TypeSyntaxPtr;
using kai::ast::UnitTypeSyntax;

namespace {

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

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

// --- UnitTypeSyntax ---

void testUnitTypeSyntaxConstruction() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "()");
    const SourceSpan span = spanOf(file, 0, 2);

    const UnitTypeSyntax type(span);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Unit);
    KAI_CHECK(type.span() == span);
    KAI_CHECK(sm.text(type.span()) == "()");
}

// --- SliceTypeSyntax ---

void testSliceTypeSyntax() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[i32]");
    const SourceSpan elementSpan = spanOf(file, 1, 4); // i32
    const SourceSpan fullSpan = spanOf(file, 0, 5);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    SliceTypeSyntax type(std::move(element), fullSpan);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Slice);
    KAI_CHECK(type.element().kind() == TypeSyntaxKind::Named);
    KAI_CHECK(sm.text(type.element().span()) == "i32");
    KAI_CHECK(type.span() == fullSpan);
    KAI_CHECK(element == nullptr);
}

// --- ArrayTypeSyntax ---

void testArrayTypeSyntax() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[i32; 4]");
    const SourceSpan elementSpan = spanOf(file, 1, 4); // i32
    const SourceSpan lengthSpan = spanOf(file, 6, 7);  // 4
    const SourceSpan fullSpan = spanOf(file, 0, 8);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    ExprPtr length = std::make_unique<LiteralExpr>(LiteralKind::Integer, lengthSpan);
    ArrayTypeSyntax type(std::move(element), std::move(length), fullSpan);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Array);
    KAI_CHECK(sm.text(type.element().span()) == "i32");
    KAI_CHECK(type.span() == fullSpan);
    KAI_CHECK(element == nullptr);
    KAI_CHECK(length == nullptr);
}

void testArrayTypeSyntaxLengthIsAnExpr() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[i32; 4]");
    const SourceSpan elementSpan = spanOf(file, 1, 4);
    const SourceSpan lengthSpan = spanOf(file, 6, 7);
    const SourceSpan fullSpan = spanOf(file, 0, 8);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    ExprPtr length = std::make_unique<LiteralExpr>(LiteralKind::Integer, lengthSpan);
    ArrayTypeSyntax type(std::move(element), std::move(length), fullSpan);

    // length() returns a plain Expr& - no numeric decoding, no special
    // "array length" node type.
    const Expr& lengthExpr = type.length();
    KAI_CHECK(lengthExpr.kind() == ExprKind::Literal);
}

void testArrayTypeSyntaxLengthIsIntegerLiteralExpr() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[i32; 4]");
    const SourceSpan elementSpan = spanOf(file, 1, 4);
    const SourceSpan lengthSpan = spanOf(file, 6, 7);
    const SourceSpan fullSpan = spanOf(file, 0, 8);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    ExprPtr length = std::make_unique<LiteralExpr>(LiteralKind::Integer, lengthSpan);
    ArrayTypeSyntax type(std::move(element), std::move(length), fullSpan);

    const auto& literal = static_cast<const LiteralExpr&>(type.length());
    KAI_CHECK(literal.literalKind() == LiteralKind::Integer);
    KAI_CHECK(sm.text(literal.span()) == "4");
}

// --- ReferenceTypeSyntax ---

void testReferenceTypeSyntaxImmutable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&i32");
    const SourceSpan operatorSpan = spanOf(file, 0, 1); // &
    const SourceSpan referentSpan = spanOf(file, 1, 4); // i32
    const SourceSpan fullSpan = spanOf(file, 0, 4);

    TypeSyntaxPtr referent = std::make_unique<NamedTypeSyntax>(Identifier{referentSpan}, referentSpan);
    ReferenceTypeSyntax type(ReferenceMutability::Immutable, operatorSpan, std::move(referent), fullSpan);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Reference);
    KAI_CHECK(type.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(sm.text(type.operatorSpan()) == "&");
    KAI_CHECK(sm.text(type.referent().span()) == "i32");
    KAI_CHECK(type.span() == fullSpan);
    KAI_CHECK(referent == nullptr);
}

void testReferenceTypeSyntaxMutable() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&mut i32");
    const SourceSpan operatorSpan = spanOf(file, 0, 4); // &mut
    const SourceSpan referentSpan = spanOf(file, 5, 8); // i32
    const SourceSpan fullSpan = spanOf(file, 0, 8);

    TypeSyntaxPtr referent = std::make_unique<NamedTypeSyntax>(Identifier{referentSpan}, referentSpan);
    ReferenceTypeSyntax type(ReferenceMutability::Mutable, operatorSpan, std::move(referent), fullSpan);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Reference);
    KAI_CHECK(type.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(sm.text(type.operatorSpan()) == "&mut");
    KAI_CHECK(type.span() == fullSpan);
}

void testReferenceTypeSyntaxOperatorSpanMayIncludeWhitespace() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "& mut i32");
    const SourceSpan operatorSpan = spanOf(file, 0, 5); // "& mut"
    const SourceSpan referentSpan = spanOf(file, 6, 9); // i32
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    TypeSyntaxPtr referent = std::make_unique<NamedTypeSyntax>(Identifier{referentSpan}, referentSpan);
    ReferenceTypeSyntax type(ReferenceMutability::Mutable, operatorSpan, std::move(referent), fullSpan);

    // Whitespace between Amp and KwMut is not significant: operatorSpan
    // still runs from Amp.begin to KwMut.end, whitespace included.
    KAI_CHECK(type.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(sm.text(type.operatorSpan()) == "& mut");
}

void testKindAndMutabilityAreDistinctNonConflictingAPIs() {
    // Regression: ReferenceTypeSyntax::kind() (inherited from
    // NodeBase<TypeSyntaxKind>, the AST-category API) and
    // ReferenceTypeSyntax::mutability() (this node's own accessor) are
    // two entirely separate members with unrelated return types - there
    // is no overload/hiding ambiguity between them.
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&mut i32");
    const SourceSpan operatorSpan = spanOf(file, 0, 4);
    const SourceSpan referentSpan = spanOf(file, 5, 8);
    const SourceSpan fullSpan = spanOf(file, 0, 8);

    TypeSyntaxPtr referent = std::make_unique<NamedTypeSyntax>(Identifier{referentSpan}, referentSpan);
    ReferenceTypeSyntax type(ReferenceMutability::Mutable, operatorSpan, std::move(referent), fullSpan);

    const TypeSyntaxKind category = type.kind();
    const ReferenceMutability mutability = type.mutability();

    KAI_CHECK(category == TypeSyntaxKind::Reference);
    KAI_CHECK(mutability == ReferenceMutability::Mutable);
}

// --- Nested reference/slice/array combinations ---

void testReferenceToSlice() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&[i32]");
    const SourceSpan operatorSpan = spanOf(file, 0, 1); // &
    const SourceSpan elementSpan = spanOf(file, 2, 5);  // i32
    const SourceSpan sliceSpan = spanOf(file, 1, 6);    // [i32]
    const SourceSpan fullSpan = spanOf(file, 0, 6);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    TypeSyntaxPtr slice = std::make_unique<SliceTypeSyntax>(std::move(element), sliceSpan);
    ReferenceTypeSyntax reference(ReferenceMutability::Immutable, operatorSpan, std::move(slice), fullSpan);

    KAI_CHECK(reference.mutability() == ReferenceMutability::Immutable);
    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Slice);
    const auto& sliceType = static_cast<const SliceTypeSyntax&>(reference.referent());
    KAI_CHECK(sm.text(sliceType.element().span()) == "i32");
    KAI_CHECK(reference.span() == fullSpan);
}

void testMutableReferenceToSlice() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&mut [i32]");
    const SourceSpan operatorSpan = spanOf(file, 0, 4); // &mut
    const SourceSpan elementSpan = spanOf(file, 6, 9);  // i32
    const SourceSpan sliceSpan = spanOf(file, 5, 10);   // [i32]
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    TypeSyntaxPtr slice = std::make_unique<SliceTypeSyntax>(std::move(element), sliceSpan);
    ReferenceTypeSyntax reference(ReferenceMutability::Mutable, operatorSpan, std::move(slice), fullSpan);

    KAI_CHECK(reference.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Slice);
    KAI_CHECK(sm.text(reference.operatorSpan()) == "&mut");
    KAI_CHECK(reference.span() == fullSpan);
}

void testReferenceToFixedArray() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&[i32; 4]");
    const SourceSpan operatorSpan = spanOf(file, 0, 1); // &
    const SourceSpan elementSpan = spanOf(file, 2, 5);  // i32
    const SourceSpan lengthSpan = spanOf(file, 7, 8);   // 4
    const SourceSpan arraySpan = spanOf(file, 1, 9);    // [i32; 4]
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    ExprPtr length = std::make_unique<LiteralExpr>(LiteralKind::Integer, lengthSpan);
    TypeSyntaxPtr array = std::make_unique<ArrayTypeSyntax>(std::move(element), std::move(length), arraySpan);
    ReferenceTypeSyntax reference(ReferenceMutability::Immutable, operatorSpan, std::move(array), fullSpan);

    KAI_CHECK(reference.referent().kind() == TypeSyntaxKind::Array);
    const auto& arrayType = static_cast<const ArrayTypeSyntax&>(reference.referent());
    KAI_CHECK(sm.text(arrayType.length().span()) == "4");
    KAI_CHECK(reference.span() == fullSpan);
}

void testNestedSliceOfFixedArray() {
    // [[i32; 4]] -> SliceTypeSyntax(ArrayTypeSyntax(i32, 4))
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "[[i32; 4]]");
    const SourceSpan innerElementSpan = spanOf(file, 2, 5); // i32
    const SourceSpan innerLengthSpan = spanOf(file, 7, 8);  // 4
    const SourceSpan innerArraySpan = spanOf(file, 1, 9);   // [i32; 4]
    const SourceSpan outerSpan = spanOf(file, 0, 10);       // [[i32; 4]]

    TypeSyntaxPtr innerElement = std::make_unique<NamedTypeSyntax>(Identifier{innerElementSpan}, innerElementSpan);
    ExprPtr innerLength = std::make_unique<LiteralExpr>(LiteralKind::Integer, innerLengthSpan);
    TypeSyntaxPtr innerArray =
        std::make_unique<ArrayTypeSyntax>(std::move(innerElement), std::move(innerLength), innerArraySpan);
    SliceTypeSyntax outer(std::move(innerArray), outerSpan);

    KAI_CHECK(outer.kind() == TypeSyntaxKind::Slice);
    KAI_CHECK(outer.element().kind() == TypeSyntaxKind::Array);
    const auto& innerArrayType = static_cast<const ArrayTypeSyntax&>(outer.element());
    KAI_CHECK(sm.text(innerArrayType.element().span()) == "i32");
    KAI_CHECK(sm.text(innerArrayType.length().span()) == "4");
    KAI_CHECK(outer.span() == outerSpan);
}

// --- GenericTypeSyntax ---

void testGenericTypeSyntaxSingleArgument() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "Option<i32>");
    const SourceSpan nameSpan = spanOf(file, 0, 6);  // Option
    const SourceSpan argSpan = spanOf(file, 7, 10);  // i32
    const SourceSpan fullSpan = spanOf(file, 0, 11); // Option<i32>

    std::vector<TypeSyntaxPtr> arguments;
    arguments.push_back(std::make_unique<NamedTypeSyntax>(Identifier{argSpan}, argSpan));
    GenericTypeSyntax type(Identifier{nameSpan}, std::move(arguments), fullSpan);

    KAI_CHECK(type.kind() == TypeSyntaxKind::Generic);
    KAI_CHECK(sm.text(type.name().span) == "Option");
    KAI_CHECK(type.arguments().size() == 1);
    KAI_CHECK(sm.text(type.arguments()[0]->span()) == "i32");
    KAI_CHECK(type.span() == fullSpan);

    // Ownership actually transferred out of the local vector.
    KAI_CHECK(arguments.empty());
}

void testGenericTypeSyntaxMultipleArgumentsOrdering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "Result<i32, E>");
    const SourceSpan nameSpan = spanOf(file, 0, 6);  // Result
    const SourceSpan arg1Span = spanOf(file, 7, 10); // i32
    const SourceSpan arg2Span = spanOf(file, 12, 13); // E
    const SourceSpan fullSpan = spanOf(file, 0, 14); // Result<i32, E>

    std::vector<TypeSyntaxPtr> arguments;
    arguments.push_back(std::make_unique<NamedTypeSyntax>(Identifier{arg1Span}, arg1Span));
    arguments.push_back(std::make_unique<NamedTypeSyntax>(Identifier{arg2Span}, arg2Span));
    GenericTypeSyntax type(Identifier{nameSpan}, std::move(arguments), fullSpan);

    KAI_CHECK(type.arguments().size() == 2);
    KAI_CHECK(sm.text(type.arguments()[0]->span()) == "i32");
    KAI_CHECK(sm.text(type.arguments()[1]->span()) == "E");
    KAI_CHECK(type.span() == fullSpan);
}

void testGenericTypeSyntaxNestedGenericArgument() {
    // Result<Option<i32>, E>
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "Result<Option<i32>, E>");
    const SourceSpan outerNameSpan = spanOf(file, 0, 6);   // Result
    const SourceSpan innerNameSpan = spanOf(file, 7, 13);  // Option
    const SourceSpan innerArgSpan = spanOf(file, 14, 17);  // i32
    const SourceSpan innerFullSpan = spanOf(file, 7, 18);  // Option<i32>
    const SourceSpan outerArg2Span = spanOf(file, 20, 21); // E
    const SourceSpan outerFullSpan = spanOf(file, 0, 22);  // Result<Option<i32>, E>

    std::vector<TypeSyntaxPtr> innerArguments;
    innerArguments.push_back(std::make_unique<NamedTypeSyntax>(Identifier{innerArgSpan}, innerArgSpan));
    TypeSyntaxPtr inner =
        std::make_unique<GenericTypeSyntax>(Identifier{innerNameSpan}, std::move(innerArguments), innerFullSpan);

    std::vector<TypeSyntaxPtr> outerArguments;
    outerArguments.push_back(std::move(inner));
    outerArguments.push_back(std::make_unique<NamedTypeSyntax>(Identifier{outerArg2Span}, outerArg2Span));
    GenericTypeSyntax outer(Identifier{outerNameSpan}, std::move(outerArguments), outerFullSpan);

    KAI_CHECK(outer.arguments().size() == 2);
    KAI_CHECK(outer.arguments()[0]->kind() == TypeSyntaxKind::Generic);

    const auto& innerType = static_cast<const GenericTypeSyntax&>(*outer.arguments()[0]);
    KAI_CHECK(sm.text(innerType.name().span) == "Option");
    KAI_CHECK(innerType.arguments().size() == 1);
    KAI_CHECK(sm.text(innerType.arguments()[0]->span()) == "i32");

    KAI_CHECK(sm.text(outer.arguments()[1]->span()) == "E");
    KAI_CHECK(outer.span() == outerFullSpan);
}

void testGenericTypeSyntaxArgumentContainingReferenceToSlice() {
    // Buffer<&mut [f32]>
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "Buffer<&mut [f32]>");
    const SourceSpan nameSpan = spanOf(file, 0, 6);        // Buffer
    const SourceSpan operatorSpan = spanOf(file, 7, 11);   // &mut
    const SourceSpan elementSpan = spanOf(file, 13, 16);   // f32
    const SourceSpan sliceSpan = spanOf(file, 12, 17);     // [f32]
    const SourceSpan referenceSpan = spanOf(file, 7, 17);  // &mut [f32]
    const SourceSpan fullSpan = spanOf(file, 0, 18);       // Buffer<&mut [f32]>

    TypeSyntaxPtr element = std::make_unique<NamedTypeSyntax>(Identifier{elementSpan}, elementSpan);
    TypeSyntaxPtr slice = std::make_unique<SliceTypeSyntax>(std::move(element), sliceSpan);
    TypeSyntaxPtr reference =
        std::make_unique<ReferenceTypeSyntax>(ReferenceMutability::Mutable, operatorSpan, std::move(slice), referenceSpan);

    std::vector<TypeSyntaxPtr> arguments;
    arguments.push_back(std::move(reference));
    GenericTypeSyntax type(Identifier{nameSpan}, std::move(arguments), fullSpan);

    KAI_CHECK(type.arguments().size() == 1);
    KAI_CHECK(type.arguments()[0]->kind() == TypeSyntaxKind::Reference);

    const auto& referenceType = static_cast<const ReferenceTypeSyntax&>(*type.arguments()[0]);
    KAI_CHECK(referenceType.mutability() == ReferenceMutability::Mutable);
    KAI_CHECK(referenceType.referent().kind() == TypeSyntaxKind::Slice);
    KAI_CHECK(type.span() == fullSpan);
}

} // namespace

int main() {
    testNamedTypeSyntaxConstruction();
    testNamedTypeSyntaxDoesNotDistinguishPrimitiveFromUserNames();

    testUnitTypeSyntaxConstruction();

    testSliceTypeSyntax();

    testArrayTypeSyntax();
    testArrayTypeSyntaxLengthIsAnExpr();
    testArrayTypeSyntaxLengthIsIntegerLiteralExpr();

    testReferenceTypeSyntaxImmutable();
    testReferenceTypeSyntaxMutable();
    testReferenceTypeSyntaxOperatorSpanMayIncludeWhitespace();
    testKindAndMutabilityAreDistinctNonConflictingAPIs();

    testReferenceToSlice();
    testMutableReferenceToSlice();
    testReferenceToFixedArray();
    testNestedSliceOfFixedArray();

    testGenericTypeSyntaxSingleArgument();
    testGenericTypeSyntaxMultipleArgumentsOrdering();
    testGenericTypeSyntaxNestedGenericArgument();
    testGenericTypeSyntaxArgumentContainingReferenceToSlice();

    return kai::test::failureCount == 0 ? 0 : 1;
}
