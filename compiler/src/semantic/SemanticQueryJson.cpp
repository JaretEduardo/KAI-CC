#include "kai/semantic/SemanticQueryJson.hpp"

#include <cstddef>
#include <string>

namespace kai::semantic {

namespace {

void appendEnvelopeHead(std::string& out, const QueryJsonEnvelope& envelope) {
    out += "{\"schemaVersion\":";
    out += std::to_string(kSemanticInspectionSchemaVersion);
    out += ",\"file\":";
    appendEscapedJsonString(out, envelope.file);
    out += ",\"query\":";
    appendPosition(out, envelope.query);
}

void appendSymbolOrNull(std::string& out, const std::optional<SemanticSymbolInfo>& symbol, const SemanticModel& model) {
    out += ",\"symbol\":";
    if (symbol.has_value()) {
        appendSymbolJson(out, *symbol, model);
    } else {
        out += "null";
    }
}

} // namespace

std::string writeDefinitionJson(const QueryJsonEnvelope& envelope, const DefinitionResult& result,
                                 const SemanticModel& model) {
    std::string out;
    appendEnvelopeHead(out, envelope);
    appendSymbolOrNull(out, result, model);
    out += '}';
    return out;
}

std::string writeReferencesJson(const QueryJsonEnvelope& envelope, const ReferencesResult& result,
                                 const SemanticModel& model) {
    std::string out;
    appendEnvelopeHead(out, envelope);
    appendSymbolOrNull(out, result.symbol, model);

    out += ",\"references\":[";
    for (std::size_t i = 0; i < result.references.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += "{\"range\":";
        appendRange(out, result.references[i]);
        out += '}';
    }
    out += "]}";
    return out;
}

} // namespace kai::semantic
