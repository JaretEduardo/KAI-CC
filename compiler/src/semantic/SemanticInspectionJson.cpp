#include "kai/semantic/SemanticInspectionJson.hpp"

#include "kai/semantic/SemanticTypeName.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace kai::semantic {

// Public (declared in SemanticInspectionJson.hpp) - reused by
// SemanticQueryJson.cpp (M2 spec §7). This compiler's source text is not
// validated as UTF-8 anywhere upstream; passing non-ASCII bytes through
// unescaped is exactly what valid UTF-8 JSON text requires anyway.
void appendEscapedJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    const int written = std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out.append(buffer, static_cast<std::size_t>(written));
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

// Public - see this function's own declaration in SemanticInspectionJson.hpp.
void appendPosition(std::string& out, const InspectionPosition& position) {
    out += "{\"line\":";
    out += std::to_string(position.line);
    out += ",\"column\":";
    out += std::to_string(position.column);
    out += '}';
}

// Public - see this function's own declaration in SemanticInspectionJson.hpp.
void appendRange(std::string& out, const InspectionRange& range) {
    out += "{\"start\":";
    appendPosition(out, range.start);
    out += ",\"end\":";
    appendPosition(out, range.end);
    out += '}';
}

namespace {

// Implementation details of appendSymbolJson() only - never needed
// outside this file.

void appendParameter(std::string& out, const SemanticParameterInfo& parameter, const SemanticModel& model) {
    out += "{\"name\":";
    appendEscapedJsonString(out, parameter.name);
    out += ",\"type\":";
    appendEscapedJsonString(out, typeName(parameter.type, model));
    out += ",\"definition\":";
    appendRange(out, parameter.definition);
    out += '}';
}

// No `default:` case: SemanticSymbolKind is fully implemented today.
std::string_view symbolKindName(SemanticSymbolKind kind) {
    switch (kind) {
        case SemanticSymbolKind::Function:
            return "function";
        case SemanticSymbolKind::Parameter:
            return "parameter";
        case SemanticSymbolKind::Local:
            return "local";
    }
    return "local";
}

} // namespace

// Public - see this function's own declaration in SemanticInspectionJson.hpp
// for why M2's definition/references JSON reuses this exact per-symbol shape.
void appendSymbolJson(std::string& out, const SemanticSymbolInfo& symbol, const SemanticModel& model) {
    out += "{\"name\":";
    appendEscapedJsonString(out, symbol.name);
    out += ",\"kind\":";
    appendEscapedJsonString(out, symbolKindName(symbol.kind));
    out += ",\"definition\":";
    appendRange(out, symbol.definition);

    if (symbol.kind == SemanticSymbolKind::Function) {
        out += ",\"parameters\":[";
        for (std::size_t i = 0; i < symbol.parameters.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            appendParameter(out, symbol.parameters[i], model);
        }
        out += "],\"returnType\":";
        appendEscapedJsonString(out, typeName(symbol.returnType, model));
    } else {
        out += ",\"type\":";
        appendEscapedJsonString(out, typeName(symbol.type, model));
        if (symbol.enclosingFunction.has_value()) {
            out += ",\"enclosingFunction\":";
            appendEscapedJsonString(out, *symbol.enclosingFunction);
        }
    }

    out += '}';
}

std::string writeSemanticInspectionJson(const SemanticInspectionResult& result, const SemanticModel& model) {
    std::string out;
    out += "{\"schemaVersion\":";
    out += std::to_string(kSemanticInspectionSchemaVersion);
    out += ",\"file\":";
    appendEscapedJsonString(out, result.file);
    out += ",\"symbols\":[";
    for (std::size_t i = 0; i < result.symbols.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendSymbolJson(out, result.symbols[i], model);
    }
    out += "]}";
    return out;
}

} // namespace kai::semantic
