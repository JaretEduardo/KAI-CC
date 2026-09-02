#include "kai/semantic/SemanticInspector.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/SemanticTypeName.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceManager;
using kai::parser::Parser;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticInspectionResult;
using kai::semantic::SemanticInspector;
using kai::semantic::SemanticModel;
using kai::semantic::SemanticSymbolInfo;
using kai::semantic::SemanticSymbolKind;
using kai::semantic::TypeChecker;
using kai::semantic::typeName;
using kai::semantic::TypeKind;

namespace {

// Real frontend pipeline, exactly like LLVMCodeGeneratorTests.cpp's own
// compileToLLVM() helper, but stopping after ControlFlowAnalyzer -
// SemanticInspector is a semantic-layer query, never a codegen consumer.
struct Inspected {
    bool ok = false;
    SemanticInspectionResult result;
    // KAI LANGUAGE M7A: kept alongside `result` (rather than discarded)
    // so a test can render an array-typed symbol's Type canonically via
    // semantic::typeName()/arrayElementType()/arrayLength() - all of
    // which require the SAME SemanticModel that produced it (see
    // Type.hpp's CompoundTypeId documentation).
    SemanticModel model;
};

Inspected inspectSource(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    if (!parsed.has_value()) {
        return {};
    }

    SemanticModel model;
    SemanticAnalyzer analyzer(sm);
    model = analyzer.analyze(*parsed);

    TypeChecker checker(sm);
    checker.check(*parsed, model);

    const ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    if (!model.errors().empty()) {
        return {};
    }

    const SemanticInspector inspector(sm, model);
    SemanticInspectionResult result = inspector.inspect(*parsed);
    return Inspected{true, std::move(result), std::move(model)};
}

const SemanticSymbolInfo* findSymbol(const SemanticInspectionResult& result, const std::string& name,
                                      SemanticSymbolKind kind) {
    for (const auto& symbol : result.symbols) {
        if (symbol.name == name && symbol.kind == kind) {
            return &symbol;
        }
    }
    return nullptr;
}

// A: function name/kind/signature/definition.
void testFunctionNameKindSignatureDefinition() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* fn = findSymbol(inspected.result, "add", SemanticSymbolKind::Function);
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    KAI_CHECK(fn->returnType.kind() == TypeKind::I64);
    KAI_CHECK(fn->parameters.size() == 2);
    // "fn add(...)": "add" starts at column 4 (1-indexed), ends
    // (half-open) at column 7.
    KAI_CHECK(fn->definition.start.line == 1);
    KAI_CHECK(fn->definition.start.column == 4);
    KAI_CHECK(fn->definition.end.line == 1);
    KAI_CHECK(fn->definition.end.column == 7);
}

// B: parameters - names/types/order, both nested and as their own flat entries.
void testParameterNamesTypesOrderAndFlatEntries() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn add(a: i64, b: u32) -> i64 {\n    return 0\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* fn = findSymbol(inspected.result, "add", SemanticSymbolKind::Function);
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    KAI_CHECK(fn->parameters.size() == 2);
    KAI_CHECK(fn->parameters[0].name == "a");
    KAI_CHECK(fn->parameters[0].type.kind() == TypeKind::I64);
    KAI_CHECK(fn->parameters[1].name == "b");
    KAI_CHECK(fn->parameters[1].type.kind() == TypeKind::U32);

    // M1 spec §12: parameters ALSO get their own flat top-level entry.
    const SemanticSymbolInfo* flatA = findSymbol(inspected.result, "a", SemanticSymbolKind::Parameter);
    KAI_CHECK(flatA != nullptr);
    if (flatA != nullptr) {
        KAI_CHECK(flatA->type.kind() == TypeKind::I64);
        KAI_CHECK(flatA->enclosingFunction.has_value());
        if (flatA->enclosingFunction.has_value()) {
            KAI_CHECK(*flatA->enclosingFunction == "add");
        }
    }
}

// C: annotated local reports the annotated semantic type.
void testAnnotatedLocalReportsSemanticType() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn main() {\n    let score: i64 = 42\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* local = findSymbol(inspected.result, "score", SemanticSymbolKind::Local);
    KAI_CHECK(local != nullptr);
    if (local != nullptr) {
        KAI_CHECK(local->type.kind() == TypeKind::I64);
        KAI_CHECK(local->enclosingFunction.has_value());
        if (local->enclosingFunction.has_value()) {
            KAI_CHECK(*local->enclosingFunction == "main");
        }
    }
}

// D: inferred local reports TypeChecker's OWN inferred type (default
// integer literal -> i32) - never re-inferred by the inspector itself.
void testInferredLocalReportsSemanticInferredType() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn main() {\n    let score = 42\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* local = findSymbol(inspected.result, "score", SemanticSymbolKind::Local);
    KAI_CHECK(local != nullptr);
    if (local != nullptr) {
        KAI_CHECK(local->type.kind() == TypeKind::I32);
    }
}

// E: multiple functions preserve SOURCE order, never alphabetical.
void testMultipleFunctionsPreserveSourceOrder() {
    SourceManager sm;
    const Inspected inspected =
        inspectSource(sm, "fn zeta() -> i64 {\n    return 1\n}\nfn alpha() -> i64 {\n    return 2\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    std::vector<std::string> functionNamesInOrder;
    for (const auto& symbol : inspected.result.symbols) {
        if (symbol.kind == SemanticSymbolKind::Function) {
            functionNamesInOrder.push_back(symbol.name);
        }
    }
    KAI_CHECK(functionNamesInOrder.size() == 2);
    if (functionNamesInOrder.size() == 2) {
        KAI_CHECK(functionNamesInOrder[0] == "zeta");
        KAI_CHECK(functionNamesInOrder[1] == "alpha");
    }
}

// F: locals declared inside if/while are discovered.
void testLocalsInsideIfAndWhileAreDiscovered() {
    SourceManager sm;
    const std::string source = "fn f(cond: bool) {\n"
                                "    if cond {\n"
                                "        let inIf: i64 = 1\n"
                                "    }\n"
                                "\n"
                                "    while cond {\n"
                                "        let inWhile: i64 = 2\n"
                                "    }\n"
                                "}";
    const Inspected inspected = inspectSource(sm, source);
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    KAI_CHECK(findSymbol(inspected.result, "inIf", SemanticSymbolKind::Local) != nullptr);
    KAI_CHECK(findSymbol(inspected.result, "inWhile", SemanticSymbolKind::Local) != nullptr);
}

// G: shadowed locals sharing one name remain TWO distinct entries with
// distinct definition spans - never deduplicated by name.
void testShadowedLocalsRemainDistinct() {
    SourceManager sm;
    const std::string source = "fn f(cond: bool) -> i64 {\n"
                                "    let x: i64 = 1\n"
                                "\n"
                                "    if cond {\n"
                                "        let x: i64 = 2\n"
                                "        return x\n"
                                "    }\n"
                                "\n"
                                "    return x\n"
                                "}";
    const Inspected inspected = inspectSource(sm, source);
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    int xCount = 0;
    std::vector<std::uint32_t> xLines;
    for (const auto& symbol : inspected.result.symbols) {
        if (symbol.name == "x" && symbol.kind == SemanticSymbolKind::Local) {
            ++xCount;
            xLines.push_back(symbol.definition.start.line);
        }
    }
    KAI_CHECK(xCount == 2);
    if (xCount == 2) {
        KAI_CHECK(xLines[0] != xLines[1]);
    }
}

// H: prelude builtins (print/panic/assert) are never exposed - they are
// not declared BY the inspected file.
void testBuiltinsAreExcluded() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn main() {\n    print(1)\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    for (const auto& symbol : inspected.result.symbols) {
        KAI_CHECK(symbol.name != "print");
        KAI_CHECK(symbol.name != "panic");
        KAI_CHECK(symbol.name != "assert");
    }
}

// I: source positions are correct - 1-indexed, half-open end.
void testSourcePositionsAreCorrect() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn add() -> i64 {\n    return 0\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* fn = findSymbol(inspected.result, "add", SemanticSymbolKind::Function);
    KAI_CHECK(fn != nullptr);
    if (fn != nullptr) {
        KAI_CHECK(fn->definition.start.line == 1);
        KAI_CHECK(fn->definition.start.column == 4);
        KAI_CHECK(fn->definition.end.line == 1);
        KAI_CHECK(fn->definition.end.column == 7);
    }
}

// The `for` loop variable is itself a discoverable Local, since
// SemanticAnalyzer declares it as a genuine SymbolKind::Local. KAI
// LANGUAGE M6: for a supported integer-range `for i in start..end`, its
// Type is now the real matched endpoint element type (i32 here, both
// endpoints being default-typed integer literals) - not "unresolved".
void testForLoopVariableIsDiscoveredAsALocal() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn f() {\n    for i in 0..10 {\n        print(i)\n    }\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* loopVar = findSymbol(inspected.result, "i", SemanticSymbolKind::Local);
    KAI_CHECK(loopVar != nullptr);
    if (loopVar != nullptr) {
        KAI_CHECK(loopVar->type.kind() == TypeKind::I32);
    }
}

// KAI LANGUAGE M7A: an array-typed local is discovered exactly like any
// other local, with its real, canonical fixed-size-array Type - never
// "unresolved", never a leaked internal CompoundTypeId, and the SAME
// canonical string ("[i32; 3]") an external tool would see via
// semantic::typeName().
void testArrayLocalReportsCanonicalArrayType() {
    SourceManager sm;
    const Inspected inspected = inspectSource(sm, "fn f() {\n    let xs = [1, 2, 3]\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* local = findSymbol(inspected.result, "xs", SemanticSymbolKind::Local);
    KAI_CHECK(local != nullptr);
    if (local != nullptr) {
        KAI_CHECK(local->type.isArray());
        if (local->type.isArray()) {
            KAI_CHECK(inspected.model.arrayElementType(local->type) == kai::semantic::Type::i32());
            KAI_CHECK(inspected.model.arrayLength(local->type) == 3);
            KAI_CHECK(typeName(local->type, inspected.model) == "[i32; 3]");
        }
    }
}

// KAI LANGUAGE M8A: array PARAMETER and RETURN types render canonically
// too - the exact same generic Symbol/FunctionSignature machinery
// already used for every other type, no ABI/implementation-detail state
// involved (SemanticInspector never queries anything backend-related).
void testArrayParameterAndReturnTypeReportCanonically() {
    SourceManager sm;
    const Inspected inspected =
        inspectSource(sm, "fn sum(xs: [i32; 3]) -> [str; 4] {\n    return [\"a\", \"b\", \"c\", \"d\"]\n}");
    KAI_CHECK(inspected.ok);
    if (!inspected.ok) {
        return;
    }

    const SemanticSymbolInfo* fn = findSymbol(inspected.result, "sum", SemanticSymbolKind::Function);
    KAI_CHECK(fn != nullptr);
    if (fn != nullptr) {
        KAI_CHECK(fn->returnType.isArray());
        if (fn->returnType.isArray()) {
            KAI_CHECK(inspected.model.arrayElementType(fn->returnType) == kai::semantic::Type::str());
            KAI_CHECK(inspected.model.arrayLength(fn->returnType) == 4);
            KAI_CHECK(typeName(fn->returnType, inspected.model) == "[str; 4]");
        }
        KAI_CHECK(fn->parameters.size() == 1);
        if (fn->parameters.size() == 1) {
            KAI_CHECK(fn->parameters[0].type.isArray());
            if (fn->parameters[0].type.isArray()) {
                KAI_CHECK(typeName(fn->parameters[0].type, inspected.model) == "[i32; 3]");
            }
        }
    }

    const SemanticSymbolInfo* param = findSymbol(inspected.result, "xs", SemanticSymbolKind::Parameter);
    KAI_CHECK(param != nullptr);
    if (param != nullptr) {
        KAI_CHECK(typeName(param->type, inspected.model) == "[i32; 3]");
    }
}

} // namespace

int main() {
    testFunctionNameKindSignatureDefinition();
    testParameterNamesTypesOrderAndFlatEntries();
    testAnnotatedLocalReportsSemanticType();
    testInferredLocalReportsSemanticInferredType();
    testMultipleFunctionsPreserveSourceOrder();
    testLocalsInsideIfAndWhileAreDiscovered();
    testShadowedLocalsRemainDistinct();
    testBuiltinsAreExcluded();
    testSourcePositionsAreCorrect();
    testForLoopVariableIsDiscoveredAsALocal();
    testArrayLocalReportsCanonicalArrayType();
    testArrayParameterAndReturnTypeReportCanonically();

    return kai::test::failureCount == 0 ? 0 : 1;
}
