#include "kai/semantic/SemanticQuery.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticInspector.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <memory>
#include <optional>
#include <string>

using kai::FileId;
using kai::SourceManager;
using kai::parser::Parser;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::DefinitionResult;
using kai::semantic::InspectionPosition;
using kai::semantic::ReferencesResult;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticModel;
using kai::semantic::SemanticQuery;
using kai::semantic::SemanticSymbolKind;
using kai::semantic::TypeChecker;
using kai::semantic::TypeKind;

namespace {

// A fully-checked SourceFile plus a ready-to-query SemanticQuery over
// it. Owns the parsed AST/model so both outlive every query call in a
// test (SemanticQuery only ever stores references - see its own class
// comment).
struct QueryFixture {
    kai::parser::ParseResult<kai::ast::SourceFile> parsed;
    SemanticModel model;
    bool ok = false;
};

std::unique_ptr<QueryFixture> makeFixture(SourceManager& sm, const std::string& source) {
    // Built via aggregate initialization with real values throughout -
    // QueryFixture is never default-constructed (ast::SourceFile, and
    // therefore std::expected<SourceFile, ParseError>, has no default
    // state to construct).
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    if (!parsed.has_value()) {
        return std::make_unique<QueryFixture>(QueryFixture{std::move(parsed), SemanticModel{}, false});
    }

    SemanticModel model;
    SemanticAnalyzer analyzer(sm);
    model = analyzer.analyze(*parsed);
    TypeChecker checker(sm);
    checker.check(*parsed, model);
    const ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    const bool ok = model.errors().empty();
    return std::make_unique<QueryFixture>(QueryFixture{std::move(parsed), std::move(model), ok});
}

InspectionPosition pos(std::uint32_t line, std::uint32_t column) {
    return InspectionPosition{line, column};
}

// A: querying AT a declaration resolves to that same declaration.
void testDeclarationPositionResolvesItself() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() {\n    let score: i64 = 42\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    let score: i64 = 42" - "score" begins at column 9.
    const DefinitionResult result = query.findDefinition(pos(2, 9));
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(result->name == "score");
        KAI_CHECK(result->kind == SemanticSymbolKind::Local);
        KAI_CHECK(result->definition.start.column == 9);
    }
}

// B: a local USE resolves to its declaration.
void testLocalUseResolvesDeclaration() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() -> i64 {\n    let x: i64 = 40\n    return x\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    return x" - the use `x` begins at column 12.
    const DefinitionResult result = query.findDefinition(pos(3, 12));
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(result->name == "x");
        // Definition must point at the DECLARATION site (line 2), not
        // the use site (line 3) that was queried.
        KAI_CHECK(result->definition.start.line == 2);
    }
}

// C: a parameter USE resolves to its declaration.
void testParameterUseResolvesDeclaration() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn double(x: i64) -> i64 {\n    return x + x\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    return x + x" - the FIRST use `x` begins at column 12.
    const DefinitionResult result = query.findDefinition(pos(2, 12));
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(result->name == "x");
        KAI_CHECK(result->kind == SemanticSymbolKind::Parameter);
        KAI_CHECK(result->definition.start.line == 1); // the parameter's own declaration line
    }
}

// D: a function call resolves to the function's declaration.
void testFunctionCallResolvesDeclaration() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() -> i64 {\n    return add(20, 22)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    return add(20, 22)" - the callee `add` begins at column 12.
    const DefinitionResult result = query.findDefinition(pos(5, 12));
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(result->name == "add");
        KAI_CHECK(result->kind == SemanticSymbolKind::Function);
        KAI_CHECK(result->definition.start.line == 1);
        KAI_CHECK(result->parameters.size() == 2);
    }
}

// E: outer/inner shadowed locals remain distinct symbols.
void testShadowedLocalsRemainDistinct() {
    SourceManager sm;
    const std::string source = "fn f(cond: bool) -> i64 {\n"
                                "    let x: i64 = 10\n"
                                "\n"
                                "    if cond {\n"
                                "        let x: i64 = 20\n"
                                "        print(x)\n"
                                "    }\n"
                                "\n"
                                "    return x\n"
                                "}";
    const auto fixture = makeFixture(sm, source);
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // Outer `x` declared at (2, 9); inner `x` declared at (5, 13).
    const DefinitionResult outerDef = query.findDefinition(pos(2, 9));
    const DefinitionResult innerDef = query.findDefinition(pos(5, 13));
    KAI_CHECK(outerDef.has_value());
    KAI_CHECK(innerDef.has_value());
    if (outerDef.has_value() && innerDef.has_value()) {
        KAI_CHECK(outerDef->definition.start.line != innerDef->definition.start.line);
    }

    // print(x) at (6, 15) must resolve to the INNER declaration.
    const DefinitionResult useInIf = query.findDefinition(pos(6, 15));
    KAI_CHECK(useInIf.has_value());
    if (useInIf.has_value()) {
        KAI_CHECK(useInIf->definition.start.line == 5);
    }

    // return x at (9, 12) must resolve to the OUTER declaration.
    const DefinitionResult useInReturn = query.findDefinition(pos(9, 12));
    KAI_CHECK(useInReturn.has_value());
    if (useInReturn.has_value()) {
        KAI_CHECK(useInReturn->definition.start.line == 2);
    }

    // references(outer x) must contain ONLY the return-statement use.
    const ReferencesResult outerRefs = query.findReferences(pos(2, 9));
    KAI_CHECK(outerRefs.references.size() == 1);
    if (outerRefs.references.size() == 1) {
        KAI_CHECK(outerRefs.references[0].start.line == 9);
    }

    // references(inner x) must contain ONLY the print(x) use.
    const ReferencesResult innerRefs = query.findReferences(pos(5, 13));
    KAI_CHECK(innerRefs.references.size() == 1);
    if (innerRefs.references.size() == 1) {
        KAI_CHECK(innerRefs.references[0].start.line == 6);
    }
}

// F: references excludes the declaration occurrence itself.
void testReferencesExcludesDeclarationOccurrence() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() -> i64 {\n    let x: i64 = 1\n    return x\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    const ReferencesResult refs = query.findReferences(pos(2, 9)); // the declaration
    KAI_CHECK(refs.references.size() == 1);
    if (refs.references.size() == 1) {
        // The one reference is the `return x` use (line 3), never the
        // declaration itself (line 2).
        KAI_CHECK(refs.references[0].start.line == 3);
    }
}

// G: an assignment target counts as a reference.
void testAssignmentTargetCountsAsReference() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn main() -> i64 {\n    let x: i64 = 40\n    mut y: i64 = x\n    y = y + x\n    return y\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "mut y: i64 = x" - y declared at (3, 9).
    const ReferencesResult yRefs = query.findReferences(pos(3, 9));
    // y = y + x -> assignment target `y` (line 4) + RHS `y` (line 4) +
    // return y (line 5) = 3 total references.
    KAI_CHECK(yRefs.references.size() == 3);

    // "let x: i64 = 40" - x declared at (2, 9).
    const ReferencesResult xRefs = query.findReferences(pos(2, 9));
    // x used in `mut y = x` (line 3) and `y + x` (line 4) = 2 references,
    // never including any `y` occurrence.
    KAI_CHECK(xRefs.references.size() == 2);
}

// H: a function call's callee counts as a reference.
void testFunctionCallCalleeCountsAsReference() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() -> i64 {\n    return add(20, 22)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    const ReferencesResult refs = query.findReferences(pos(1, 4)); // "add"'s own declaration
    KAI_CHECK(refs.references.size() == 1);
    if (refs.references.size() == 1) {
        KAI_CHECK(refs.references[0].start.line == 5);
    }
}

// I: a whitespace/blank-line position returns no symbol.
void testWhitespacePositionReturnsNoSymbol() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() {\n    let x: i64 = 1\n\n    print(x)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    const DefinitionResult def = query.findDefinition(pos(3, 1)); // the blank line
    KAI_CHECK(!def.has_value());

    const ReferencesResult refs = query.findReferences(pos(3, 1));
    KAI_CHECK(!refs.symbol.has_value());
    KAI_CHECK(refs.references.empty());
}

// J: the end-column half-open boundary does not match.
void testEndColumnBoundaryDoesNotMatch() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() {\n    let x: i64 = 1\n    print(x)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "x" spans columns [9, 10). Column 9 matches; column 10 (the
    // half-open end) must NOT.
    KAI_CHECK(query.findDefinition(pos(2, 9)).has_value());
    KAI_CHECK(!query.findDefinition(pos(2, 10)).has_value());
}

// K: occurrences nested inside if/while are traversed.
void testNestedIfWhileOccurrencesAreTraversed() {
    SourceManager sm;
    const std::string source = "fn f(cond: bool) {\n"
                                "    mut total: i64 = 0\n"
                                "\n"
                                "    if cond {\n"
                                "        total = total + 1\n"
                                "    }\n"
                                "\n"
                                "    while cond {\n"
                                "        total = total + 2\n"
                                "    }\n"
                                "}";
    const auto fixture = makeFixture(sm, source);
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    mut total: i64 = 0" - total declared at (2, 9).
    const ReferencesResult refs = query.findReferences(pos(2, 9));
    // if-body: target + RHS (2 uses); while-body: target + RHS (2 uses) = 4.
    KAI_CHECK(refs.references.size() == 4);
}

// L: a user function shadowing the `print` builtin resolves to the user
// Function declaration, not the (source-less) builtin.
void testUserFunctionShadowingBuiltinResolvesUserFunction() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn print(x: i64) -> i64 {\n    return x\n}\nfn main() -> i64 {\n    return print(5)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    return print(5)" - the callee `print` begins at column 12.
    const DefinitionResult result = query.findDefinition(pos(5, 12));
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(result->kind == SemanticSymbolKind::Function);
        KAI_CHECK(result->definition.start.line == 1); // the USER function's own declaration, not a builtin
    }
}

// Bonus: querying an UNSHADOWED builtin call resolves to no symbol - a
// Builtin has no source declaration to report (M2 spec §16).
void testUnshadowedBuiltinCallResolvesToNoSymbol() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() {\n    print(1)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticQuery query(sm, fixture->model, *fixture->parsed);

    // "    print(1)" - the callee `print` begins at column 5.
    const DefinitionResult result = query.findDefinition(pos(2, 5));
    KAI_CHECK(!result.has_value());
}

} // namespace

int main() {
    testDeclarationPositionResolvesItself();
    testLocalUseResolvesDeclaration();
    testParameterUseResolvesDeclaration();
    testFunctionCallResolvesDeclaration();
    testShadowedLocalsRemainDistinct();
    testReferencesExcludesDeclarationOccurrence();
    testAssignmentTargetCountsAsReference();
    testFunctionCallCalleeCountsAsReference();
    testWhitespacePositionReturnsNoSymbol();
    testEndColumnBoundaryDoesNotMatch();
    testNestedIfWhileOccurrencesAreTraversed();
    testUserFunctionShadowingBuiltinResolvesUserFunction();
    testUnshadowedBuiltinCallResolvesToNoSymbol();

    return kai::test::failureCount == 0 ? 0 : 1;
}
