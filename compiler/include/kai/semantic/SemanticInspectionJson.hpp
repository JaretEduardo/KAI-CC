#pragma once

#include "kai/semantic/SemanticInspector.hpp"

#include <string>

namespace kai::semantic {

/// SEMANTIC INSPECTION MILESTONE 1: the numeric schema version of the
/// JSON shape `writeSemanticInspectionJson()` produces - an external
/// tooling-contract version, deliberately INDEPENDENT of the compiler's
/// own version (M1 spec §6). Bump this, never the compiler version, the
/// day this shape changes in a way an existing consumer would need to
/// react to.
inline constexpr int kSemanticInspectionSchemaVersion = 1;

/// Serializes `result` to the versioned JSON schema:
///
///     {
///       "schemaVersion": 1,
///       "file": "...",
///       "symbols": [
///         { "name": "...", "kind": "function", "definition": {...},
///           "parameters": [ { "name": "...", "type": "...",
///                              "definition": {...} }, ... ],
///           "returnType": "..." },
///         { "name": "...", "kind": "parameter"|"local",
///           "definition": {...}, "type": "...",
///           "enclosingFunction": "..." (omitted if absent) },
///         ...
///       ]
///     }
///
/// where a `definition`/nested `definition` is
/// `{"start":{"line":L,"column":C},"end":{"line":L,"column":C}}`
/// (1-indexed, half-open - see InspectionRange's own documentation).
///
/// Deterministic: the same SemanticInspectionResult always serializes to
/// byte-identical output (object key order is fixed by this function,
/// array order is exactly `result.symbols`' own order - see
/// SemanticInspector's own source-declaration-order guarantee). Every
/// string value (file name, symbol/parameter names) is safely JSON-
/// escaped - see SemanticInspectionJson.cpp's own escaping helper.
///
/// Produces compact (non-pretty-printed) JSON with NO trailing newline -
/// callers decide their own output framing (e.g. a CLI command appending
/// exactly one `\n` before writing to stdout).
std::string writeSemanticInspectionJson(const SemanticInspectionResult& result);

} // namespace kai::semantic
