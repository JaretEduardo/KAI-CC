#include "kai/cli/SemanticCallQueryCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/SemanticErrorFormat.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticCallQuery.hpp"
#include "kai/semantic/SemanticCallQueryJson.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"

#include <string>

namespace kai::cli {

namespace {

// Shared by both entry points below: load/parse/check the real frontend,
// returning false (having already reported the failure to `err` and
// guaranteed no bytes were written to `out`) on any error - the SAME
// policy CompileCommand/InspectCommand/SemanticQueryCommand already
// established.
bool runFrontend(SourceManager& sources, const std::filesystem::path& inputPath, std::ostream& err,
                  std::optional<FileId>& outFile, std::optional<ast::SourceFile>& outParsed,
                  semantic::SemanticModel& outModel, int& outExitCodeOnFailure) {
    const auto loaded = sources.loadFile(inputPath);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << inputPath.string() << "': " << loaded.error().message() << '\n';
        outExitCodeOnFailure = 2;
        return false;
    }
    outFile = *loaded;

    parser::Parser parser(sources, *loaded);
    auto parsed = parser.parseSourceFile();
    if (!parsed) {
        err << formatParseError(sources, parsed.error()) << '\n';
        outExitCodeOnFailure = 4;
        return false;
    }
    outParsed.emplace(std::move(*parsed));

    semantic::SemanticAnalyzer analyzer(sources);
    outModel = analyzer.analyze(*outParsed);

    semantic::TypeChecker checker(sources);
    checker.check(*outParsed, outModel);

    const semantic::ControlFlowAnalyzer flow;
    flow.check(*outParsed, outModel);

    if (!outModel.errors().empty()) {
        for (const semantic::SemanticError& error : outModel.errors()) {
            err << formatSemanticError(sources, error) << '\n';
        }
        outExitCodeOnFailure = 5;
        return false;
    }

    return true;
}

} // namespace

int runCallQueryCommand(CallQueryKind kind, SourceManager& sources, const std::filesystem::path& inputPath,
                         std::uint32_t line, std::uint32_t column, std::ostream& out, std::ostream& err) {
    std::optional<FileId> file;
    std::optional<ast::SourceFile> parsed;
    semantic::SemanticModel model;
    int failureExitCode = 0;

    if (!runFrontend(sources, inputPath, err, file, parsed, model, failureExitCode)) {
        return failureExitCode;
    }

    const semantic::SemanticCallQuery query(sources, model, *parsed);
    const semantic::InspectionPosition position{line, column};
    const semantic::CallQueryJsonEnvelope envelope{std::string(sources.fileName(*file)), position};

    if (kind == CallQueryKind::Callers) {
        out << semantic::writeCallRelationJson(envelope, query.findCallers(position), "callers") << '\n';
    } else {
        out << semantic::writeCallRelationJson(envelope, query.findCallees(position), "callees") << '\n';
    }
    return 0;
}

int runCallGraphCommand(SourceManager& sources, const std::filesystem::path& inputPath, std::ostream& out,
                         std::ostream& err) {
    std::optional<FileId> file;
    std::optional<ast::SourceFile> parsed;
    semantic::SemanticModel model;
    int failureExitCode = 0;

    if (!runFrontend(sources, inputPath, err, file, parsed, model, failureExitCode)) {
        return failureExitCode;
    }

    const semantic::SemanticCallQuery query(sources, model, *parsed);
    out << semantic::writeCallGraphJson(std::string(sources.fileName(*file)), query.directCallGraph()) << '\n';
    return 0;
}

} // namespace kai::cli
