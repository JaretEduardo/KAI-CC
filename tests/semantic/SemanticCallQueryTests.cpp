#include "kai/semantic/SemanticCallQuery.hpp"

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
using kai::semantic::CallGraph;
using kai::semantic::CallRelationResult;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::InspectionPosition;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticCallQuery;
using kai::semantic::SemanticModel;
using kai::semantic::TypeChecker;

namespace {

// Same fixture shape as SemanticQueryTests.cpp's own QueryFixture - a
// fully-checked SourceFile/SemanticModel the test outlives every query
// call against.
struct QueryFixture {
    kai::parser::ParseResult<kai::ast::SourceFile> parsed;
    SemanticModel model;
    bool ok = false;
};

std::unique_ptr<QueryFixture> makeFixture(SourceManager& sm, const std::string& source) {
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

// A: a simple caller/callee relationship.
void testSimpleCallerCallee() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn a() -> i64 {\n    return b()\n}\nfn b() -> i64 {\n    return 1\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4)); // "a"
    KAI_CHECK(callees.function.has_value());
    KAI_CHECK(callees.relations.size() == 1);
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "b");
        KAI_CHECK(callees.relations[0].callSites.size() == 1);
    }

    const CallRelationResult callers = query.findCallers(pos(4, 4)); // "b"
    KAI_CHECK(callers.relations.size() == 1);
    if (callers.relations.size() == 1) {
        KAI_CHECK(callers.relations[0].function.name == "a");
    }
}

// B: a forward call (callee declared AFTER the caller in source).
void testForwardCall() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn main() -> i64 {\n    return answer()\n}\nfn answer() -> i64 {\n    return 42\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4)); // "main"
    KAI_CHECK(callees.relations.size() == 1);
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "answer");
    }
}

// C: multiple call sites to the SAME function are grouped, not lost.
void testMultipleCallSitesToSameFunctionAreGrouped() {
    SourceManager sm;
    const auto fixture = makeFixture(
        sm, "fn f() {\n    a()\n    a()\n}\nfn a() {\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4)); // "f"
    KAI_CHECK(callees.relations.size() == 1); // ONE group for "a" ...
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "a");
        KAI_CHECK(callees.relations[0].callSites.size() == 2); // ... with BOTH call sites retained
        KAI_CHECK(callees.relations[0].callSites[0].start.line == 2);
        KAI_CHECK(callees.relations[0].callSites[1].start.line == 3);
    }
}

// D: multiple DISTINCT callees.
void testMultipleDistinctCallees() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn f() {\n    a()\n    b()\n}\nfn a() {\n}\nfn b() {\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4));
    KAI_CHECK(callees.relations.size() == 2);
    if (callees.relations.size() == 2) {
        KAI_CHECK(callees.relations[0].function.name == "a"); // first-occurrence order
        KAI_CHECK(callees.relations[1].function.name == "b");
    }
}

// E: a parameterized function call still resolves correctly.
void testParameterizedFunctionCall() {
    SourceManager sm;
    const auto fixture = makeFixture(
        sm, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() -> i64 {\n    return add(20, 22)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callers = query.findCallers(pos(1, 4)); // "add"
    KAI_CHECK(callers.relations.size() == 1);
    if (callers.relations.size() == 1) {
        KAI_CHECK(callers.relations[0].function.name == "main");
    }
}

// F: nested calls - `return outer(inner())` produces TWO edges.
void testNestedCalls() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn current() -> i64 {\n    return outer(inner())\n}\n"
                                          "fn outer(x: i64) -> i64 {\n    return x\n}\n"
                                          "fn inner() -> i64 {\n    return 1\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4)); // "current"
    KAI_CHECK(callees.relations.size() == 2);
    if (callees.relations.size() == 2) {
        KAI_CHECK(callees.relations[0].function.name == "outer");
        KAI_CHECK(callees.relations[1].function.name == "inner");
    }
}

// G: a call inside an `if` condition/body is still a direct edge.
void testCallInsideIf() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn cond() -> bool {\n    return true\n}\n"
                        "fn body() {\n}\n"
                        "fn f() {\n    if cond() {\n        body()\n    }\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(6, 4)); // "f"
    KAI_CHECK(callees.relations.size() == 2);
    if (callees.relations.size() == 2) {
        KAI_CHECK(callees.relations[0].function.name == "cond"); // condition, encountered first
        KAI_CHECK(callees.relations[1].function.name == "body");
    }
}

// H: a call inside a `while` condition/body is still a direct edge.
void testCallInsideWhile() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn cond() -> bool {\n    return true\n}\n"
                        "fn body() {\n}\n"
                        "fn f() {\n    while cond() {\n        body()\n    }\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(6, 4));
    KAI_CHECK(callees.relations.size() == 2);
}

// I: self recursion - no special-casing should be needed.
void testSelfRecursion() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn recurse(n: i64) -> i64 {\n    return recurse(n)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(1, 4));
    KAI_CHECK(callees.relations.size() == 1);
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "recurse");
    }

    const CallRelationResult callers = query.findCallers(pos(1, 4));
    KAI_CHECK(callers.relations.size() == 1);
    if (callers.relations.size() == 1) {
        KAI_CHECK(callers.relations[0].function.name == "recurse");
    }
}

// J: mutual recursion - direct only, no infinite traversal.
void testMutualRecursion() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn a(x: i64) -> i64 {\n    return b(x)\n}\nfn b(x: i64) -> i64 {\n    return a(x)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult calleesOfA = query.findCallees(pos(1, 4));
    KAI_CHECK(calleesOfA.relations.size() == 1);
    if (calleesOfA.relations.size() == 1) {
        KAI_CHECK(calleesOfA.relations[0].function.name == "b");
    }

    const CallRelationResult callersOfA = query.findCallers(pos(1, 4));
    KAI_CHECK(callersOfA.relations.size() == 1);
    if (callersOfA.relations.size() == 1) {
        KAI_CHECK(callersOfA.relations[0].function.name == "b"); // b calls a
    }
}

// K: an un-shadowed builtin `print` call is EXCLUDED from the graph.
void testBuiltinPrintExcluded() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn helper() -> i64 {\n    return 1\n}\nfn main() {\n    print(helper())\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(4, 4)); // "main"
    // ONLY "helper" - never an edge to the builtin "print".
    KAI_CHECK(callees.relations.size() == 1);
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "helper");
    }
}

// L: a user-defined `print` shadowing the builtin IS included.
void testUserDefinedPrintIncluded() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn print(x: i64) -> i64 {\n    return x\n}\nfn main() {\n    print(5)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult callees = query.findCallees(pos(4, 4)); // "main"
    KAI_CHECK(callees.relations.size() == 1);
    if (callees.relations.size() == 1) {
        KAI_CHECK(callees.relations[0].function.name == "print");
        KAI_CHECK(callees.relations[0].function.definition.start.line == 1); // the USER declaration
    }
}

// M: querying the declaration vs. a call site gives the SAME target.
void testDeclarationAndCallSiteGiveSameTarget() {
    SourceManager sm;
    const auto fixture =
        makeFixture(sm, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() -> i64 {\n    return add(1, 2)\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    // "    return add(1, 2)" - "add" begins at column 12.
    const CallRelationResult fromDeclaration = query.findCallers(pos(1, 4));
    const CallRelationResult fromCallSite = query.findCallers(pos(5, 12));
    KAI_CHECK(fromDeclaration.function.has_value());
    KAI_CHECK(fromCallSite.function.has_value());
    if (fromDeclaration.function.has_value() && fromCallSite.function.has_value()) {
        KAI_CHECK(fromDeclaration.function->name == fromCallSite.function->name);
        KAI_CHECK(fromDeclaration.function->definition.start.line == fromCallSite.function->definition.start.line);
    }
}

// N: querying a Local/Parameter (non-function) position gives no function target.
void testNonFunctionQueryGivesNoFunction() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn f(x: i64) -> i64 {\n    let y: i64 = x\n    return y\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    // Parameter "x" at (1, 6).
    const CallRelationResult onParameter = query.findCallers(pos(1, 6));
    KAI_CHECK(!onParameter.function.has_value());
    KAI_CHECK(onParameter.relations.empty());

    // Local "y" at (2, 9).
    const CallRelationResult onLocal = query.findCallees(pos(2, 9));
    KAI_CHECK(!onLocal.function.has_value());
    KAI_CHECK(onLocal.relations.empty());
}

// O: deterministic ordering - repeated queries produce identical results.
void testDeterministicOrdering() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn f() {\n    a()\n    b()\n    a()\n}\nfn a() {\n}\nfn b() {\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallRelationResult first = query.findCallees(pos(1, 4));
    const CallRelationResult second = query.findCallees(pos(1, 4));
    KAI_CHECK(first.relations.size() == second.relations.size());
    for (std::size_t i = 0; i < first.relations.size(); ++i) {
        KAI_CHECK(first.relations[i].function.name == second.relations[i].function.name);
        KAI_CHECK(first.relations[i].callSites.size() == second.relations[i].callSites.size());
    }
}

// P: a zero-callee function still appears as a node in the whole graph.
void testZeroCalleeFunctionAppearsInGraph() {
    SourceManager sm;
    const auto fixture = makeFixture(sm, "fn leaf() -> i64 {\n    return 1\n}\nfn f() {\n    leaf()\n}");
    KAI_CHECK(fixture->ok);
    if (!fixture->ok) {
        return;
    }
    const SemanticCallQuery query(sm, fixture->model, *fixture->parsed);

    const CallGraph graph = query.directCallGraph();
    KAI_CHECK(graph.functions.size() == 2);
    if (graph.functions.size() == 2) {
        KAI_CHECK(graph.functions[0].function.name == "leaf");
        KAI_CHECK(graph.functions[0].callees.empty()); // present, just empty
        KAI_CHECK(graph.functions[1].function.name == "f");
        KAI_CHECK(graph.functions[1].callees.size() == 1);
    }
}

} // namespace

int main() {
    testSimpleCallerCallee();
    testForwardCall();
    testMultipleCallSitesToSameFunctionAreGrouped();
    testMultipleDistinctCallees();
    testParameterizedFunctionCall();
    testNestedCalls();
    testCallInsideIf();
    testCallInsideWhile();
    testSelfRecursion();
    testMutualRecursion();
    testBuiltinPrintExcluded();
    testUserDefinedPrintIncluded();
    testDeclarationAndCallSiteGiveSameTarget();
    testNonFunctionQueryGivesNoFunction();
    testDeterministicOrdering();
    testZeroCalleeFunctionAppearsInGraph();

    return kai::test::failureCount == 0 ? 0 : 1;
}
