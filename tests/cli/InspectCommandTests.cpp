#include "kai/cli/InspectCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticInspectionJson.hpp"
#include "kai/semantic/SemanticInspector.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using kai::SourceManager;

namespace {

std::filesystem::path writeTempSource(const std::string& baseName, const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / baseName;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return path;
}

// A + B: a successful inspect exits 0 and writes ONLY JSON (plus one
// trailing newline) to stdout - no "Inspecting..." chatter, nothing on
// stderr.
void testSuccessfulInspectProducesJsonOnlyOnStdout() {
    const std::filesystem::path path =
        writeTempSource("kai_inspect_ok.kai",
                         "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\nfn main() {\n    print(add(1, 2))\n}");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int exitCode = kai::cli::runInspectCommand(sm, path, out, err);

    KAI_CHECK(exitCode == 0);
    KAI_CHECK(err.str().empty());

    const std::string stdoutText = out.str();
    KAI_CHECK(!stdoutText.empty());
    KAI_CHECK(!stdoutText.empty() && stdoutText.back() == '\n');
    // Exactly one newline in the whole output - the JSON itself is a
    // single line, followed by exactly one trailing newline.
    KAI_CHECK(stdoutText.find('\n') == stdoutText.size() - 1);

    KAI_CHECK(stdoutText.find("\"schemaVersion\":1") != std::string::npos);
    KAI_CHECK(stdoutText.find("\"file\":") != std::string::npos);
    KAI_CHECK(stdoutText.find("\"symbols\":[") != std::string::npos);
    KAI_CHECK(stdoutText.find("\"name\":\"add\"") != std::string::npos);
    KAI_CHECK(stdoutText.find("\"kind\":\"function\"") != std::string::npos);
    KAI_CHECK(stdoutText.find("\"returnType\":\"i64\"") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// C: the same input, inspected twice (even via two independent
// SourceManager instances), produces byte-identical output.
void testSameInputInspectedTwiceIsByteIdentical() {
    const std::filesystem::path path =
        writeTempSource("kai_inspect_determinism.kai", "fn main() {\n    let x: i64 = 1\n    print(x)\n}");

    SourceManager sm1;
    std::ostringstream out1;
    std::ostringstream err1;
    const int exit1 = kai::cli::runInspectCommand(sm1, path, out1, err1);

    SourceManager sm2;
    std::ostringstream out2;
    std::ostringstream err2;
    const int exit2 = kai::cli::runInspectCommand(sm2, path, out2, err2);

    KAI_CHECK(exit1 == 0);
    KAI_CHECK(exit2 == 0);
    KAI_CHECK(out1.str() == out2.str());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// D: a parse-error input produces NO JSON on stdout and a nonzero exit.
void testParseErrorProducesNoJsonAndNonzeroExit() {
    const std::filesystem::path path = writeTempSource("kai_inspect_parse_error.kai", "fn f( {\n");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int exitCode = kai::cli::runInspectCommand(sm, path, out, err);

    KAI_CHECK(exitCode != 0);
    KAI_CHECK(out.str().empty());
    KAI_CHECK(!err.str().empty());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// E: a semantic/type error input produces NO JSON on stdout and a
// nonzero exit - M1's "no partial-inspection mode" policy.
void testSemanticErrorProducesNoJsonAndNonzeroExit() {
    const std::filesystem::path path =
        writeTempSource("kai_inspect_semantic_error.kai", "fn main() {\n    print(undefined_var)\n}");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int exitCode = kai::cli::runInspectCommand(sm, path, out, err);

    KAI_CHECK(exitCode != 0);
    KAI_CHECK(out.str().empty());
    KAI_CHECK(!err.str().empty());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// F: a source file identity requiring JSON escaping is encoded safely.
// A real KAI identifier can never contain a quote/backslash (the lexer
// only accepts ordinary identifier characters), but the file's own
// display name (SourceManager::fileName()) is an arbitrary string - see
// SemanticInspectionResult::file's own documentation - so it is the
// practical place this milestone's escaping actually gets exercised.
// Uses SemanticInspector/writeSemanticInspectionJson directly (rather
// than the file-loading CLI path) so the crafted name doesn't also have
// to be a valid, portable real filename.
void testFileNameRequiringEscapingIsEncodedSafely() {
    SourceManager sm;
    const std::string trickyName = R"(weird"name\with.kai)";
    const kai::FileId file = sm.addVirtualFile(trickyName, "fn main() {\n}");

    kai::parser::Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    KAI_CHECK(parsed.has_value());
    if (!parsed.has_value()) {
        return;
    }

    kai::semantic::SemanticModel model;
    kai::semantic::SemanticAnalyzer analyzer(sm);
    model = analyzer.analyze(*parsed);
    kai::semantic::TypeChecker checker(sm);
    checker.check(*parsed, model);
    const kai::semantic::ControlFlowAnalyzer flow;
    flow.check(*parsed, model);
    KAI_CHECK(model.errors().empty());
    if (!model.errors().empty()) {
        return;
    }

    const kai::semantic::SemanticInspector inspector(sm, model);
    const kai::semantic::SemanticInspectionResult result = inspector.inspect(*parsed);
    const std::string json = kai::semantic::writeSemanticInspectionJson(result, model);

    // Both the embedded quote and the embedded backslash must be
    // escaped - a naive concatenation would otherwise produce invalid,
    // unparseable JSON at exactly this point.
    KAI_CHECK(json.find(R"("file":"weird\"name\\with.kai")") != std::string::npos);
}

} // namespace

int main() {
    testSuccessfulInspectProducesJsonOnlyOnStdout();
    testSameInputInspectedTwiceIsByteIdentical();
    testParseErrorProducesNoJsonAndNonzeroExit();
    testSemanticErrorProducesNoJsonAndNonzeroExit();
    testFileNameRequiringEscapingIsEncodedSafely();

    return kai::test::failureCount == 0 ? 0 : 1;
}
