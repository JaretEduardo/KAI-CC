#include "kai/cli/SemanticCallQueryCommand.hpp"

#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using kai::SourceManager;
using kai::cli::CallQueryKind;

namespace {

std::filesystem::path writeTempSource(const std::string& baseName, const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / baseName;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return path;
}

struct QueryOutcome {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

QueryOutcome runCallQuery(CallQueryKind kind, const std::filesystem::path& path, std::uint32_t line,
                           std::uint32_t column) {
    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    QueryOutcome outcome;
    outcome.exitCode = kai::cli::runCallQueryCommand(kind, sm, path, line, column, out, err);
    outcome.stdoutText = out.str();
    outcome.stderrText = err.str();
    return outcome;
}

QueryOutcome runCallGraph(const std::filesystem::path& path) {
    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    QueryOutcome outcome;
    outcome.exitCode = kai::cli::runCallGraphCommand(sm, path, out, err);
    outcome.stdoutText = out.str();
    outcome.stderrText = err.str();
    return outcome;
}

void expectJsonOnlyOnSuccess(const QueryOutcome& outcome) {
    KAI_CHECK(outcome.exitCode == 0);
    KAI_CHECK(outcome.stderrText.empty());
    KAI_CHECK(!outcome.stdoutText.empty());
    KAI_CHECK(!outcome.stdoutText.empty() && outcome.stdoutText.back() == '\n');
    KAI_CHECK(outcome.stdoutText.find('\n') == outcome.stdoutText.size() - 1);
}

// --- CALLERS ---

void testCallersSimpleFunction() {
    const std::filesystem::path path =
        writeTempSource("kai_callers_simple.kai", "fn a() -> i64 {\n    return b()\n}\nfn b() -> i64 {\n    return 1\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callers, path, 4, 4); // "b"
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"function\":{\"name\":\"b\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"callers\":[{\"function\":{\"name\":\"a\"") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCallersDuplicateCallSitesGrouped() {
    const std::filesystem::path path = writeTempSource("kai_callers_dup.kai", "fn f() {\n    a()\n    a()\n}\nfn a() {\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callers, path, 5, 4); // "a"
    expectJsonOnlyOnSuccess(outcome);
    // Exactly one caller group ("f"), with TWO callSites retained.
    KAI_CHECK(outcome.stdoutText.find("\"callers\":[{\"function\":{\"name\":\"f\"") != std::string::npos);
    std::size_t callSitesPos = outcome.stdoutText.find("\"callSites\":[");
    KAI_CHECK(callSitesPos != std::string::npos);
    if (callSitesPos != std::string::npos) {
        int rangeCount = 0;
        std::size_t pos = callSitesPos;
        const std::size_t end = outcome.stdoutText.find(']', callSitesPos);
        while (pos < end && (pos = outcome.stdoutText.find("\"start\"", pos)) != std::string::npos && pos < end) {
            ++rangeCount;
            pos += 1;
        }
        KAI_CHECK(rangeCount == 2);
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCallersRecursion() {
    const std::filesystem::path path = writeTempSource("kai_callers_recursion.kai", "fn recurse(n: i64) -> i64 {\n    return recurse(n)\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callers, path, 1, 4); // "recurse"
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"callers\":[{\"function\":{\"name\":\"recurse\"") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCallersNonFunctionGivesNull() {
    const std::filesystem::path path = writeTempSource("kai_callers_none.kai", "fn f(x: i64) -> i64 {\n    return x\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callers, path, 1, 6); // parameter "x"
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"function\":null") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"callers\":[]") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// --- CALLEES ---

void testCalleesMultiple() {
    const std::filesystem::path path = writeTempSource("kai_callees_multi.kai", "fn f() {\n    a()\n    b()\n}\nfn a() {\n}\nfn b() {\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callees, path, 1, 4); // "f"
    expectJsonOnlyOnSuccess(outcome);
    const std::size_t aPos = outcome.stdoutText.find("\"name\":\"a\"");
    const std::size_t bPos = outcome.stdoutText.find("\"name\":\"b\"");
    KAI_CHECK(aPos != std::string::npos && bPos != std::string::npos && aPos < bPos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCalleesForwardAndNested() {
    const std::filesystem::path path = writeTempSource(
        "kai_callees_forward_nested.kai",
        "fn current() -> i64 {\n    return outer(inner())\n}\nfn outer(x: i64) -> i64 {\n    return x\n}\nfn inner() -> "
        "i64 {\n    return 1\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callees, path, 1, 4); // "current" (forward calls)
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"outer\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"inner\"") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCalleesBuiltinExcluded() {
    const std::filesystem::path path =
        writeTempSource("kai_callees_builtin.kai", "fn helper() -> i64 {\n    return 1\n}\nfn main() {\n    print(helper())\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callees, path, 4, 4); // "main"
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"helper\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"print\"") == std::string::npos); // never a builtin edge

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testCalleesUserShadowingBuiltin() {
    const std::filesystem::path path =
        writeTempSource("kai_callees_shadow.kai", "fn print(x: i64) -> i64 {\n    return x\n}\nfn main() {\n    print(5)\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callees, path, 4, 4); // "main"
    expectJsonOnlyOnSuccess(outcome);
    // The USER "print" (declared at line 1) must appear as a real callee.
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"print\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"definition\":{\"start\":{\"line\":1") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// --- SHARED ---

void testRepeatedQueryByteIdentical() {
    const std::filesystem::path path =
        writeTempSource("kai_call_determinism.kai", "fn a() -> i64 {\n    return b()\n}\nfn b() -> i64 {\n    return 1\n}");
    const QueryOutcome first = runCallQuery(CallQueryKind::Callees, path, 1, 4);
    const QueryOutcome second = runCallQuery(CallQueryKind::Callees, path, 1, 4);
    KAI_CHECK(first.exitCode == 0);
    KAI_CHECK(second.exitCode == 0);
    KAI_CHECK(first.stdoutText == second.stdoutText);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testInvalidSourceGivesNoJson() {
    const std::filesystem::path path = writeTempSource("kai_call_invalid.kai", "fn main() {\n    print(undefined_var)\n}");
    const QueryOutcome outcome = runCallQuery(CallQueryKind::Callees, path, 1, 4);
    KAI_CHECK(outcome.exitCode != 0);
    KAI_CHECK(outcome.stdoutText.empty());
    KAI_CHECK(!outcome.stderrText.empty());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// --- CALL GRAPH ---

void testCallGraphIncludesZeroCalleeFunctionsInSourceOrder() {
    const std::filesystem::path path = writeTempSource(
        "kai_call_graph.kai", "fn leaf() -> i64 {\n    return 1\n}\nfn middle() -> i64 {\n    return leaf()\n}\nfn main() {\n    "
                              "print(middle())\n}");
    const QueryOutcome outcome = runCallGraph(path);
    expectJsonOnlyOnSuccess(outcome);

    const std::size_t leafPos = outcome.stdoutText.find("\"name\":\"leaf\"");
    const std::size_t middlePos = outcome.stdoutText.find("\"name\":\"middle\"");
    const std::size_t mainPos = outcome.stdoutText.find("\"name\":\"main\"");
    KAI_CHECK(leafPos != std::string::npos && middlePos != std::string::npos && mainPos != std::string::npos);
    KAI_CHECK(leafPos < middlePos && middlePos < mainPos); // exact source-order node ordering

    // "leaf" is a zero-callee node - it must still appear with "callees":[].
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"leaf\"") != std::string::npos);
    const std::size_t leafCalleesPos = outcome.stdoutText.find("\"callees\":[]", leafPos);
    KAI_CHECK(leafCalleesPos != std::string::npos && leafCalleesPos < middlePos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    testCallersSimpleFunction();
    testCallersDuplicateCallSitesGrouped();
    testCallersRecursion();
    testCallersNonFunctionGivesNull();

    testCalleesMultiple();
    testCalleesForwardAndNested();
    testCalleesBuiltinExcluded();
    testCalleesUserShadowingBuiltin();

    testRepeatedQueryByteIdentical();
    testInvalidSourceGivesNoJson();

    testCallGraphIncludesZeroCalleeFunctionsInSourceOrder();

    return kai::test::failureCount == 0 ? 0 : 1;
}
