#include "kai/cli/InspectCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/SemanticErrorFormat.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticInspectionJson.hpp"
#include "kai/semantic/SemanticInspector.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"

namespace kai::cli {

int runInspectCommand(SourceManager& sources, const std::filesystem::path& inputPath, std::ostream& out,
                       std::ostream& err) {
    const auto loaded = sources.loadFile(inputPath);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << inputPath.string() << "': " << loaded.error().message() << '\n';
        return 2;
    }

    parser::Parser parser(sources, *loaded);
    const auto parsed = parser.parseSourceFile();
    if (!parsed) {
        err << formatParseError(sources, parsed.error()) << '\n';
        return 4;
    }

    semantic::SemanticModel model;
    semantic::SemanticAnalyzer analyzer(sources);
    model = analyzer.analyze(*parsed);

    semantic::TypeChecker checker(sources);
    checker.check(*parsed, model);

    const semantic::ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    // M1 spec §25: no partial-inspection mode - any frontend error at
    // all means no JSON is emitted, matching runCompileCommand()'s own
    // established policy for the exact same three passes.
    if (!model.errors().empty()) {
        for (const semantic::SemanticError& error : model.errors()) {
            err << formatSemanticError(sources, error, model) << '\n';
        }
        return 5;
    }

    const semantic::SemanticInspector inspector(sources, model);
    const semantic::SemanticInspectionResult result = inspector.inspect(*parsed);

    // stdout gets EXACTLY the JSON plus one trailing newline - no other
    // text is ever written to `out` on this path (M1 spec §27).
    out << semantic::writeSemanticInspectionJson(result, model) << '\n';
    return 0;
}

} // namespace kai::cli
