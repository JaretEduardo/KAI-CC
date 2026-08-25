#include "kai/semantic/SemanticCallQueryJson.hpp"

#include <cstddef>

namespace kai::semantic {

namespace {

void appendCallSiteGroup(std::string& out, const CallSiteGroup& group) {
    out += "{\"function\":";
    appendSymbolJson(out, group.function);
    out += ",\"callSites\":[";
    for (std::size_t i = 0; i < group.callSites.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendRange(out, group.callSites[i]);
    }
    out += "]}";
}

void appendCallSiteGroupArray(std::string& out, const std::vector<CallSiteGroup>& groups) {
    out += '[';
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendCallSiteGroup(out, groups[i]);
    }
    out += ']';
}

} // namespace

std::string writeCallRelationJson(const CallQueryJsonEnvelope& envelope, const CallRelationResult& result,
                                   std::string_view relationKey) {
    std::string out;
    out += "{\"schemaVersion\":";
    out += std::to_string(kSemanticInspectionSchemaVersion);
    out += ",\"file\":";
    appendEscapedJsonString(out, envelope.file);
    out += ",\"query\":";
    appendPosition(out, envelope.query);
    out += ",\"function\":";
    if (result.function.has_value()) {
        appendSymbolJson(out, *result.function);
    } else {
        out += "null";
    }
    out += ",\"";
    out += relationKey; // a fixed internal literal ("callers"/"callees") - never untrusted input
    out += "\":";
    appendCallSiteGroupArray(out, result.relations);
    out += '}';
    return out;
}

std::string writeCallGraphJson(const std::string& file, const CallGraph& graph) {
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
        appendSymbolJson(out, node.function);
        out += ",\"callees\":";
        appendCallSiteGroupArray(out, node.callees);
        out += '}';
    }
    out += "]}";
    return out;
}

} // namespace kai::semantic
