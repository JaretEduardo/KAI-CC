// KAI LANGUAGE M10A: compound-type foundation coverage for slice TYPE
// resolution (SemanticAnalyzer::resolveSliceTypeSyntax(), SemanticModel's
// own slice compound-type interner - see Type.hpp's TypeKind::Slice/
// CompoundTypeId documentation). Mirrors ArrayTypeTests.cpp's own
// structure and discipline exactly: SemanticAnalyzer-only (no
// TypeChecker needed - `let xs: [T] = ...`'s ANNOTATION is resolved
// entirely by SemanticAnalyzer::declareLocal() -> resolveTypeSyntax(),
// before TypeChecker ever runs), and deliberately does NOT call
// SemanticModel::internSlice() directly - like every other SemanticModel
// mutator, it is friend-only - every fact here is observed through the
// real, public pipeline: a real `.kai` source annotation, analyzed by
// the real SemanticAnalyzer, read back via the public
// symbol()/sliceElementType() API only.
//
// M10A is a TYPE-FOUNDATION-ONLY milestone: no LLVM lowering, no
// array-to-slice conversion, no slice indexing, and no slice literals
// exist - this file covers ONLY that a slice TypeSyntax resolves to a
// real, canonical, correctly-structured semantic Type. Native/codegen
// coverage of the backend's clean-failure behavior for a slice-typed
// program lives in LLVMCodeGeneratorTests.cpp instead.

#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticTypeName.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::FunctionDecl;
using kai::ast::VarDeclStmt;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticModel;
using kai::semantic::Type;
using kai::semantic::TypeKind;

namespace {

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

// Reaches into `fn f() { let xs: <annotation> = 0 }`'s single local's
// resolved Type - the initializer `0` is irrelevant here (SemanticAnalyzer
// alone never checks it against the annotation; that is TypeChecker's
// job, deliberately not exercised in this file).
Type declaredLocalType(SourceManager& sm, const std::string& annotation, SemanticModel& outModel) {
    Analyzed result = analyze(sm, "fn f() {\n    let xs: " + annotation + " = 0\n}");
    if (!result.parsed.has_value()) {
        outModel = std::move(result.model);
        return Type::error();
    }
    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    const Type type = id.has_value() ? result.model.symbol(*id).type : Type::error();
    outModel = std::move(result.model);
    return type;
}

// A. `[i32]` resolves to a real TypeKind::Slice with no SemanticError.
void testSliceTypeSyntaxResolvesToSliceKind() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(type.kind() == TypeKind::Slice);
    KAI_CHECK(type.isSlice());
    KAI_CHECK(!type.isArray());
    KAI_CHECK(!type.isError());
    KAI_CHECK(!type.isUnresolved());
}

// B. Element type extraction is exactly what the syntax spelled out.
void testSliceElementType() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32]", model);

    KAI_CHECK(type.isSlice());
    if (type.isSlice()) {
        KAI_CHECK(model.sliceElementType(type) == Type::i32());
    }
}

void testSliceElementTypeCanBeUnsigned() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[u8]", model);

    KAI_CHECK(type.isSlice());
    if (type.isSlice()) {
        KAI_CHECK(model.sliceElementType(type) == Type::u8());
    }
}

void testSliceElementTypeCanBeStr() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[str]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(type.isSlice());
    if (type.isSlice()) {
        KAI_CHECK(model.sliceElementType(type) == Type::str());
    }
}

// C. Canonicalization: two SEPARATE `[i32]` annotations, written
// independently, resolve to the exact same interned Type (spec §9/§13) -
// `[i32] == [i32]` regardless of being written twice.
void testEquivalentSliceTypesFromSeparateAnnotationsCompareEqual() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32] = 0\n"
                                   "    let b: [i32] = 0\n"
                                   "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto idA = result.model.declarationSymbol(declA.name());
    const auto idB = result.model.declarationSymbol(declB.name());
    KAI_CHECK(idA.has_value());
    KAI_CHECK(idB.has_value());
    if (idA && idB) {
        KAI_CHECK(result.model.symbol(*idA).type == result.model.symbol(*idB).type);
    }
}

// D. Inequality by element type: `[i32] != [u32]` - real, distinct Types,
// never both collapsed to one fabricated "slice" stand-in.
void testDifferentElementTypeSlicesAreDistinctWithinOneModel() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32] = 0\n"
                                   "    let b: [u32] = 0\n"
                                   "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto idA = result.model.declarationSymbol(declA.name());
    const auto idB = result.model.declarationSymbol(declB.name());
    KAI_CHECK(idA.has_value());
    KAI_CHECK(idB.has_value());
    if (idA && idB) {
        KAI_CHECK(result.model.symbol(*idA).type != result.model.symbol(*idB).type);
    }
}

// E. Slice vs. Array: `[i32]` and `[i32; 3]` are distinct Types (spec
// §2/§13) - a slice is never accidentally equal to (or interchangeable
// with) a fixed-size array of the same element type.
void testSliceIsDistinctFromArrayOfSameElementType() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32] = 0\n"
                                   "    let b: [i32; 3] = 0\n"
                                   "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& declA = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto& declB = static_cast<const VarDeclStmt&>(*fn.body().statements()[1]);
    const auto idA = result.model.declarationSymbol(declA.name());
    const auto idB = result.model.declarationSymbol(declB.name());
    KAI_CHECK(idA.has_value());
    KAI_CHECK(idB.has_value());
    if (idA && idB) {
        const Type sliceType = result.model.symbol(*idA).type;
        const Type arrayType = result.model.symbol(*idB).type;
        KAI_CHECK(sliceType.isSlice());
        KAI_CHECK(arrayType.isArray());
        KAI_CHECK(sliceType != arrayType);
    }
}

// F. An unknown element type propagates as Error, not a fabricated slice
// Type - no second, redundant diagnostic about the slice itself.
void testUnknownElementTypePropagatesAsError() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let xs: [Foo] = 0\n}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }

    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == kai::semantic::SemanticErrorKind::UnknownType);
    }

    const auto& fn = static_cast<const FunctionDecl&>(*result.parsed->declarations()[0]);
    const auto& decl = static_cast<const VarDeclStmt&>(*fn.body().statements()[0]);
    const auto id = result.model.declarationSymbol(decl.name());
    KAI_CHECK(id.has_value());
    if (id) {
        KAI_CHECK(!result.model.symbol(*id).type.isSlice());
        KAI_CHECK(result.model.symbol(*id).type.isError());
    }
}

// G. A still-deferred element shape (a reference) makes the whole slice
// stay Unresolved too - no fabricated partial Type, and no diagnostic
// invented for a shape this phase doesn't model yet.
void testDeferredElementTypeKeepsSliceUnresolved() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[&i32]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(!type.isSlice());
    KAI_CHECK(!type.isError());
    KAI_CHECK(type.isUnresolved());
}

// H. Nested fixed-array element: `[[i32; 3]]` is a slice OF arrays -
// resolves recursively with no special casing.
void testSliceOfFixedArrayElementResolves() {
    SourceManager sm;
    SemanticModel model;
    const Type outer = declaredLocalType(sm, "[[i32; 3]]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(outer.isSlice());
    if (outer.isSlice()) {
        const Type inner = model.sliceElementType(outer);
        KAI_CHECK(inner.isArray());
        if (inner.isArray()) {
            KAI_CHECK(model.arrayElementType(inner) == Type::i32());
            KAI_CHECK(model.arrayLength(inner) == 3);
        }
    }
}

// I. Nested slice element: `[[i32]]` is a slice OF slices - resolves
// recursively with no special casing (spec §9/§29).
void testNestedSliceOfSliceElementResolves() {
    SourceManager sm;
    SemanticModel model;
    const Type outer = declaredLocalType(sm, "[[i32]]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(outer.isSlice());
    if (outer.isSlice()) {
        const Type inner = model.sliceElementType(outer);
        KAI_CHECK(inner.isSlice());
        if (inner.isSlice()) {
            KAI_CHECK(model.sliceElementType(inner) == Type::i32());
        }
    }
}

// J. An array element containing a slice: `[[i32]; 3]` is a fixed array
// OF slices - the two compound kinds compose in either direction with no
// special casing needed at either level.
void testArrayOfSliceElementResolves() {
    SourceManager sm;
    SemanticModel model;
    const Type outer = declaredLocalType(sm, "[[i32]; 3]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(outer.isArray());
    if (outer.isArray()) {
        KAI_CHECK(model.arrayLength(outer) == 3);
        const Type inner = model.arrayElementType(outer);
        KAI_CHECK(inner.isSlice());
        if (inner.isSlice()) {
            KAI_CHECK(model.sliceElementType(inner) == Type::i32());
        }
    }
}

// K. Canonical type name rendering (spec §12): "[i32]", "[str]",
// "[[i32; 3]]", "[[i32]]" - recursive SemanticTypeName rendering, no
// internal TypeId/CompoundTypeId leakage.
void testCanonicalSliceTypeNameRendering() {
    SourceManager sm;
    SemanticModel model;
    const Type i32Slice = declaredLocalType(sm, "[i32]", model);
    KAI_CHECK(kai::semantic::typeName(i32Slice, model) == "[i32]");

    SourceManager sm2;
    SemanticModel model2;
    const Type strSlice = declaredLocalType(sm2, "[str]", model2);
    KAI_CHECK(kai::semantic::typeName(strSlice, model2) == "[str]");

    SourceManager sm3;
    SemanticModel model3;
    const Type sliceOfArray = declaredLocalType(sm3, "[[i32; 3]]", model3);
    KAI_CHECK(kai::semantic::typeName(sliceOfArray, model3) == "[[i32; 3]]");

    SourceManager sm4;
    SemanticModel model4;
    const Type sliceOfSlice = declaredLocalType(sm4, "[[i32]]", model4);
    KAI_CHECK(kai::semantic::typeName(sliceOfSlice, model4) == "[[i32]]");
}

// L. Function PARAMETER semantic type: `fn f(xs: [i32])` gives `xs` a
// real Slice Type (spec §6/§29) - type correctness only, NOT a claim
// this is executable (see LLVMCodeGeneratorTests.cpp for the backend
// clean-failure coverage).
void testFunctionParameterSliceType() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f(xs: [i32]) {}");
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
    const auto& signature = *result.model.symbol(*id).signature;
    KAI_CHECK(signature.parameterTypes.size() == 1);
    if (signature.parameterTypes.size() == 1) {
        KAI_CHECK(signature.parameterTypes[0].isSlice());
        KAI_CHECK(result.model.sliceElementType(signature.parameterTypes[0]) == Type::i32());
    }
}

// M. Function RETURN semantic type: `fn identity(xs: [i32]) -> [i32]` is
// semantically well-typed at the SIGNATURE level (spec §24) - this test
// deliberately does NOT check the function body/return statement (that
// is TypeChecker's job, exercised via a full compile in
// LLVMCodeGeneratorTests.cpp instead), only that the declared return
// annotation itself resolves to a real Slice Type.
void testFunctionReturnSliceType() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn identity(xs: [i32]) -> [i32] {}");
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
    const auto& signature = *result.model.symbol(*id).signature;
    KAI_CHECK(signature.returnType.isSlice());
    if (signature.returnType.isSlice()) {
        KAI_CHECK(result.model.sliceElementType(signature.returnType) == Type::i32());
        // Parameter and return Slice types are the SAME structural
        // shape (`[i32]`), so canonicalization gives them the exact
        // same interned Type - proving internSlice() canonicalizes
        // across a signature's parameter/return positions, not just
        // across separate local annotations (see this file's own
        // testEquivalentSliceTypesFromSeparateAnnotationsCompareEqual()).
        KAI_CHECK(signature.parameterTypes[0] == signature.returnType);
    }
}

} // namespace

int main() {
    testSliceTypeSyntaxResolvesToSliceKind();
    testSliceElementType();
    testSliceElementTypeCanBeUnsigned();
    testSliceElementTypeCanBeStr();
    testEquivalentSliceTypesFromSeparateAnnotationsCompareEqual();
    testDifferentElementTypeSlicesAreDistinctWithinOneModel();
    testSliceIsDistinctFromArrayOfSameElementType();
    testUnknownElementTypePropagatesAsError();
    testDeferredElementTypeKeepsSliceUnresolved();
    testSliceOfFixedArrayElementResolves();
    testNestedSliceOfSliceElementResolves();
    testArrayOfSliceElementResolves();
    testCanonicalSliceTypeNameRendering();
    testFunctionParameterSliceType();
    testFunctionReturnSliceType();

    return kai::test::failureCount == 0 ? 0 : 1;
}
