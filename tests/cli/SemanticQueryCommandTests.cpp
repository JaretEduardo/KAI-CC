#include "kai/cli/SemanticQueryCommand.hpp"

#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using kai::SourceManager;
using kai::cli::SemanticQueryKind;

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

QueryOutcome runQuery(SemanticQueryKind kind, const std::filesystem::path& path, std::uint32_t line,
                       std::uint32_t column) {
    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    QueryOutcome outcome;
    outcome.exitCode = kai::cli::runSemanticQueryCommand(kind, sm, path, line, column, out, err);
    outcome.stdoutText = out.str();
    outcome.stderrText = err.str();
    return outcome;
}

void expectJsonOnlyOnSuccess(const QueryOutcome& outcome) {
    KAI_CHECK(outcome.exitCode == 0);
    KAI_CHECK(outcome.stderrText.empty());
    KAI_CHECK(!outcome.stdoutText.empty());
    KAI_CHECK(!outcome.stdoutText.empty() && outcome.stdoutText.back() == '\n');
    KAI_CHECK(outcome.stdoutText.find('\n') == outcome.stdoutText.size() - 1); // exactly one line + trailing newline
}

// --- DEFINITION ---

// A: definition at a declaration.
void testDefinitionAtDeclaration() {
    const std::filesystem::path path = writeTempSource("kai_def_decl.kai", "fn main() {\n    let score: i64 = 42\n}");
    // "    let score: i64 = 42" - "score" begins at column 9.
    const QueryOutcome outcome = runQuery(SemanticQueryKind::Definition, path, 2, 9);
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"schemaVersion\":1") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"query\":{\"line\":2,\"column\":9}") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"score\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"kind\":\"local\"") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// B: definition at a use.
void testDefinitionAtUse() {
    const std::filesystem::path path =
        writeTempSource("kai_def_use.kai", "fn main() -> i64 {\n    let x: i64 = 1\n    return x\n}");
    // "    return x" - the use begins at column 12.
    const QueryOutcome outcome = runQuery(SemanticQueryKind::Definition, path, 3, 12);
    expectJsonOnlyOnSuccess(outcome);
    // The reported definition range must point at the DECLARATION site
    // (line 2), never the queried use site (line 3).
    KAI_CHECK(outcome.stdoutText.find("\"name\":\"x\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"definition\":{\"start\":{\"line\":2") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// C: no-symbol position -> exit 0, JSON symbol:null.
void testDefinitionNoSymbolPosition() {
    const std::filesystem::path path = writeTempSource("kai_def_none.kai", "fn main() {\n\n    print(1)\n}");
    const QueryOutcome outcome = runQuery(SemanticQueryKind::Definition, path, 2, 1); // the blank line
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"symbol\":null") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// E: frontend-invalid source -> nonzero exit, no JSON stdout.
void testDefinitionFrontendErrorProducesNoJson() {
    const std::filesystem::path path =
        writeTempSource("kai_def_frontend_error.kai", "fn main() {\n    print(undefined_var)\n}");
    const QueryOutcome outcome = runQuery(SemanticQueryKind::Definition, path, 2, 11);
    KAI_CHECK(outcome.exitCode != 0);
    KAI_CHECK(outcome.stdoutText.empty());
    KAI_CHECK(!outcome.stderrText.empty());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// F: deterministic repeated output.
void testDefinitionDeterministic() {
    const std::filesystem::path path =
        writeTempSource("kai_def_determinism.kai", "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}");
    const QueryOutcome first = runQuery(SemanticQueryKind::Definition, path, 1, 4);
    const QueryOutcome second = runQuery(SemanticQueryKind::Definition, path, 1, 4);
    KAI_CHECK(first.exitCode == 0);
    KAI_CHECK(second.exitCode == 0);
    KAI_CHECK(first.stdoutText == second.stdoutText);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// --- REFERENCES ---

// A: a local's expected reference count/ranges.
void testReferencesLocalCountAndRanges() {
    const std::filesystem::path path =
        writeTempSource("kai_refs_local.kai", "fn main() -> i64 {\n    let x: i64 = 40\n    return x\n}");
    // "    let x: i64 = 40" - x declared at column 9.
    const QueryOutcome outcome = runQuery(SemanticQueryKind::References, path, 2, 9);
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"references\":[{\"range\":{\"start\":{\"line\":3,\"column\":12}") !=
              std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// B: parameter references.
void testReferencesParameter() {
    const std::filesystem::path path = writeTempSource("kai_refs_param.kai", "fn double(x: i64) -> i64 {\n    return x + x\n}");
    // "fn double(x: i64)" - x declared at column 11.
    const QueryOutcome outcome = runQuery(SemanticQueryKind::References, path, 1, 11);
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"kind\":\"parameter\"") != std::string::npos);
    // Two uses of `x` in "return x + x".
    const std::size_t firstRange = outcome.stdoutText.find("\"references\":[");
    KAI_CHECK(firstRange != std::string::npos);
    if (firstRange != std::string::npos) {
        int rangeCount = 0;
        std::size_t pos = firstRange;
        while ((pos = outcome.stdoutText.find("\"range\":", pos)) != std::string::npos) {
            ++rangeCount;
            pos += 1;
        }
        KAI_CHECK(rangeCount == 2);
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// C: function call references.
void testReferencesFunctionCall() {
    const std::filesystem::path path = writeTempSource(
        "kai_refs_call.kai",
        "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() -> i64 {\n    return add(20, 22)\n}");
    // "fn add(...)" - "add" declared at column 4.
    const QueryOutcome outcome = runQuery(SemanticQueryKind::References, path, 1, 4);
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"kind\":\"function\"") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"references\":[{\"range\":{\"start\":{\"line\":5,\"column\":12}") !=
              std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// D: shadowed same-name locals produce SEPARATE reference sets.
void testReferencesShadowedLocalsAreSeparate() {
    const std::filesystem::path path = writeTempSource("kai_refs_shadow.kai",
                                                         "fn f(cond: bool) -> i64 {\n"
                                                         "    let x: i64 = 10\n"
                                                         "\n"
                                                         "    if cond {\n"
                                                         "        let x: i64 = 20\n"
                                                         "        print(x)\n"
                                                         "    }\n"
                                                         "\n"
                                                         "    return x\n"
                                                         "}");

    // Outer x (2, 9): only the `return x` reference (line 9).
    const QueryOutcome outerRefs = runQuery(SemanticQueryKind::References, path, 2, 9);
    expectJsonOnlyOnSuccess(outerRefs);
    KAI_CHECK(outerRefs.stdoutText.find("\"line\":9") != std::string::npos);
    KAI_CHECK(outerRefs.stdoutText.find("\"line\":6") == std::string::npos); // never the inner print(x) use

    // Inner x (5, 13): only the `print(x)` reference (line 6).
    const QueryOutcome innerRefs = runQuery(SemanticQueryKind::References, path, 5, 13);
    expectJsonOnlyOnSuccess(innerRefs);
    KAI_CHECK(innerRefs.stdoutText.find("\"line\":6") != std::string::npos);
    KAI_CHECK(innerRefs.stdoutText.find("\"line\":9") == std::string::npos); // never the outer return use

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// E: no-symbol position -> symbol:null + empty references.
void testReferencesNoSymbolPosition() {
    const std::filesystem::path path = writeTempSource("kai_refs_none.kai", "fn main() {\n\n    print(1)\n}");
    const QueryOutcome outcome = runQuery(SemanticQueryKind::References, path, 2, 1);
    expectJsonOnlyOnSuccess(outcome);
    KAI_CHECK(outcome.stdoutText.find("\"symbol\":null") != std::string::npos);
    KAI_CHECK(outcome.stdoutText.find("\"references\":[]") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// F: deterministic ordering/output.
void testReferencesDeterministic() {
    const std::filesystem::path path = writeTempSource(
        "kai_refs_determinism.kai", "fn main() -> i64 {\n    let x: i64 = 40\n    mut y: i64 = x\n    y = y + x\n    return y\n}");
    const QueryOutcome first = runQuery(SemanticQueryKind::References, path, 2, 9);
    const QueryOutcome second = runQuery(SemanticQueryKind::References, path, 2, 9);
    KAI_CHECK(first.exitCode == 0);
    KAI_CHECK(second.exitCode == 0);
    KAI_CHECK(first.stdoutText == second.stdoutText);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    testDefinitionAtDeclaration();
    testDefinitionAtUse();
    testDefinitionNoSymbolPosition();
    testDefinitionFrontendErrorProducesNoJson();
    testDefinitionDeterministic();

    testReferencesLocalCountAndRanges();
    testReferencesParameter();
    testReferencesFunctionCall();
    testReferencesShadowedLocalsAreSeparate();
    testReferencesNoSymbolPosition();
    testReferencesDeterministic();

    return kai::test::failureCount == 0 ? 0 : 1;
}
