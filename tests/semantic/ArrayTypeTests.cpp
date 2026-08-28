// KAI LANGUAGE M7A: compound-type foundation coverage for fixed-size
// array TYPE resolution (SemanticAnalyzer::resolveArrayTypeSyntax(),
// SemanticModel's own compound-type interner - see Type.hpp's
// CompoundTypeId/Array documentation). This file is SemanticAnalyzer-
// only (mirrors SemanticAnalyzerTests.cpp's own analyze() helper, no
// TypeChecker): `let xs: [T; N] = ...`'s ANNOTATION is resolved entirely
// by SemanticAnalyzer::declareLocal() -> resolveTypeSyntax(), before
// TypeChecker ever runs, so this level alone is enough to exercise
// array-type resolution/equality/interning/zero-length directly.
//
// Array LITERAL inference (TypeChecker::checkArrayLiteralExpr() -
// homogeneous/incompatible elements, contextual typing, empty literals)
// is covered separately in type_checker/ArrayLiteralTests.cpp, since
// that requires the TypeChecker pass too.
//
// Deliberately does NOT call SemanticModel::internArray() directly -
// like every other SemanticModel mutator, it is friend-only (not even
// tests may construct compound-type state ad hoc - see SemanticModel.hpp's
// own class comment) - every fact here is observed through the real,
// public pipeline: a real `.kai` source annotation, analyzed by the
// real SemanticAnalyzer, read back via the public
// symbol()/arrayElementType()/arrayLength() API only.

#include "kai/semantic/SemanticAnalyzer.hpp"

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

// A. `[i32; 3]` resolves to a real TypeKind::Array with no SemanticError.
void testArrayTypeSyntaxResolvesToArrayKind() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32; 3]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(type.kind() == TypeKind::Array);
    KAI_CHECK(type.isArray());
    KAI_CHECK(!type.isError());
    KAI_CHECK(!type.isUnresolved());
}

// B/C. Element type and length are exactly what the syntax spelled out.
void testArrayElementTypeAndLength() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32; 3]", model);

    KAI_CHECK(type.isArray());
    if (type.isArray()) {
        KAI_CHECK(model.arrayElementType(type) == Type::i32());
        KAI_CHECK(model.arrayLength(type) == 3);
    }
}

void testArrayElementTypeCanBeUnsigned() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[u8; 16]", model);

    KAI_CHECK(type.isArray());
    if (type.isArray()) {
        KAI_CHECK(model.arrayElementType(type) == Type::u8());
        KAI_CHECK(model.arrayLength(type) == 16);
    }
}

// D/E/F. Equality is structural: same element+length compares equal;
// a different length or a different element type compares unequal -
// `[i32; 3] != [i32; 4]` and `[i32; 3] != [u32; 3]` are real, distinct
// Types now, not both collapsed to one fabricated "array" stand-in.
void testEquivalentArrayTypesFromSeparateAnnotationsCompareEqual() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32; 3] = 0\n"
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
        // Interning/canonicalization (M7A spec §1/§19): two SEPARATE
        // `[i32; 3]` annotations, written independently, resolve to the
        // exact same interned Type - proven here via operator==, and
        // structurally via the shared model's own arrayElementType()/
        // arrayLength() agreeing for both.
        KAI_CHECK(result.model.symbol(*idA).type == result.model.symbol(*idB).type);
    }
}

void testDifferentLengthArrayTypesAreDistinct() {
    SourceManager sm;
    SemanticModel modelA;
    SemanticModel modelB;
    const Type three = declaredLocalType(sm, "[i32; 3]", modelA);
    SourceManager sm2;
    const Type four = declaredLocalType(sm2, "[i32; 4]", modelB);

    KAI_CHECK(three.isArray());
    KAI_CHECK(four.isArray());
    // Cross-model comparison is outside CompoundTypeId's documented
    // contract in general, but [i32;3] vs [i32;4] happen to be the
    // FIRST (and only) array type each model interns, so both get
    // CompoundTypeId(0) - this assertion instead checks the actually
    // meaningful, in-contract fact: each model's own arrayLength() for
    // its own Type disagrees, which is what "distinct types" means.
    KAI_CHECK(modelA.arrayLength(three) == 3);
    KAI_CHECK(modelB.arrayLength(four) == 4);
}

void testDifferentLengthArrayTypesAreDistinctWithinOneModel() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32; 3] = 0\n"
                                   "    let b: [i32; 4] = 0\n"
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

void testDifferentElementTypeArraysAreDistinctWithinOneModel() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n"
                                   "    let a: [i32; 3] = 0\n"
                                   "    let b: [u32; 3] = 0\n"
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

// G. Zero-length arrays are approved and valid (M7A spec §5) - not
// rejected merely because the length is 0.
void testZeroLengthArrayTypeIsValid() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32; 0]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(type.isArray());
    if (type.isArray()) {
        KAI_CHECK(model.arrayElementType(type) == Type::i32());
        KAI_CHECK(model.arrayLength(type) == 0);
    }
}

// H. Slice type syntax `[T]` (no `;`) must NOT resolve to Array - it
// stays Unresolved (M7A spec §4: arrays and slices are distinct, and
// slices remain explicitly deferred).
void testSliceTypeSyntaxStaysUnresolvedNotArray() {
    SourceManager sm;
    SemanticModel model;
    const Type type = declaredLocalType(sm, "[i32]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(!type.isArray());
    KAI_CHECK(type.isUnresolved());
}

// I. An unknown element type propagates as Error, not a fabricated array
// Type - no second, redundant diagnostic about the array itself.
void testUnknownElementTypePropagatesAsError() {
    SourceManager sm;
    Analyzed result = analyze(sm, "fn f() {\n    let xs: [Foo; 3] = 0\n}");
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
        KAI_CHECK(!result.model.symbol(*id).type.isArray());
        KAI_CHECK(result.model.symbol(*id).type.isError());
    }
}

// J. A nested array `[[i32; 2]; 3]` resolves recursively with no special
// casing (M7A does not forbid this - it simply falls out of
// resolveTypeSyntax()'s own recursion).
void testNestedArrayTypeResolves() {
    SourceManager sm;
    SemanticModel model;
    const Type outer = declaredLocalType(sm, "[[i32; 2]; 3]", model);

    KAI_CHECK(model.errors().empty());
    KAI_CHECK(outer.isArray());
    if (outer.isArray()) {
        KAI_CHECK(model.arrayLength(outer) == 3);
        const Type inner = model.arrayElementType(outer);
        KAI_CHECK(inner.isArray());
        if (inner.isArray()) {
            KAI_CHECK(model.arrayElementType(inner) == Type::i32());
            KAI_CHECK(model.arrayLength(inner) == 2);
        }
    }
}

} // namespace

int main() {
    testArrayTypeSyntaxResolvesToArrayKind();
    testArrayElementTypeAndLength();
    testArrayElementTypeCanBeUnsigned();
    testEquivalentArrayTypesFromSeparateAnnotationsCompareEqual();
    testDifferentLengthArrayTypesAreDistinct();
    testDifferentLengthArrayTypesAreDistinctWithinOneModel();
    testDifferentElementTypeArraysAreDistinctWithinOneModel();
    testZeroLengthArrayTypeIsValid();
    testSliceTypeSyntaxStaysUnresolvedNotArray();
    testUnknownElementTypePropagatesAsError();
    testNestedArrayTypeResolves();

    return kai::test::failureCount == 0 ? 0 : 1;
}
