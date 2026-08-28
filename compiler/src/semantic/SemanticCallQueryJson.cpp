#include "kai/semantic/SemanticCallQueryJson.hpp"

#include <cstddef>

namespace kai::semantic {

namespace {

void appendCallSiteGroup(std::string& out, const CallSiteGroup& group, const SemanticModel& model) {
    out += "{\"function\":";
    appendSymbolJson(out, group.function, model);
    out += ",\"callSites\":[";
    for (std::size_t i = 0; i < group.callSites.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendRange(out, group.callSites[i]);
    }
    out += "]}";
}

void appendCallSiteGroupArray(std::string& out, const std::vector<CallSiteGroup>& groups, const SemanticModel& model) {
    out += '[';
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendCallSiteGroup(out, groups[i], model);
    }
    out += ']';
}

} // namespace

std::string writeCallRelationJson(const CallQueryJsonEnvelope& envelope, const CallRelationResult& result,
                                   std::string_view relationKey, const SemanticModel& model) {
    std::string out;
    out += "{\"schemaVersion\":";
    out += std::to_string(kSemanticInspectionSchemaVersion);
    out += ",\"file\":";
    appendEscapedJsonString(out, envelope.file);
    out += ",\"query\":";
    appendPosition(out, envelope.query);
    out += ",\"function\":";
    if (result.function.has_value()) {
        appendSymbolJson(out, *result.function, model);
    } else {
        out += "null";
    }
    out += ",\"";
    out += relationKey; // a fixed internal literal ("callers"/"callees") - never untrusted input
    out += "\":";
    appendCallSiteGroupArray(out, result.relations, model);
    out += '}';
    return out;
}

std::string writeCallGraphJson(const std::string& file, const CallGraph& graph, const SemanticModel& model) {
    std::string out;
    out += "{\"schemaVersion\":";
    out += std::to_string(kSemanticInspectionSchemaVersion);
    out += ",\"file\":";
    appendEscapedJsonString(out, file);
    out += ",\"functions\":[";
    for (std::size_t i = 0; i < graph.functions.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        const CallGraphNode& node = graph.functions[i];
        out += "{\"function\":";
        appendSymbolJson(out, node.function, model);
        out += ",\"callees\":";
        appendCallSiteGroupArray(out, node.callees, model);
        out += '}';
    }
    out += "]}";
    return out;
}

} // namespace kai::semantic
