#include "kai/cli/SemanticQueryCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/SemanticErrorFormat.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticInspector.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/SemanticQuery.hpp"
#include "kai/semantic/SemanticQueryJson.hpp"
#include "kai/semantic/TypeChecker.hpp"

#include <string>

namespace kai::cli {

int runSemanticQueryCommand(SemanticQueryKind kind, SourceManager& sources, const std::filesystem::path& inputPath,
                             std::uint32_t line, std::uint32_t column, std::ostream& out, std::ostream& err) {
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

    // M2 spec §20: no partial-query mode - any frontend error at all
    // means no JSON is emitted, matching runInspectCommand()'s own
    // established policy for the exact same three passes.
    if (!model.errors().empty()) {
        for (const semantic::SemanticError& error : model.errors()) {
            err << formatSemanticError(sources, error) << '\n';
        }
        return 5;
    }

    const semantic::SemanticQuery query(sources, model, *parsed);
    const semantic::InspectionPosition position{line, column};
    const semantic::QueryJsonEnvelope envelope{std::string(sources.fileName(*loaded)), position};

    // stdout gets EXACTLY the JSON plus one trailing newline - no other
    // text is ever written to `out` on this path (M1/M2 spec §27).
    if (kind == SemanticQueryKind::Definition) {
        out << semantic::writeDefinitionJson(envelope, query.findDefinition(position)) << '\n';
    } else {
        out << semantic::writeReferencesJson(envelope, query.findReferences(position)) << '\n';
    }
    return 0;
}

} // namespace kai::cli
