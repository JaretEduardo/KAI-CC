#pragma once

#include "kai/semantic/SemanticCallQuery.hpp"
#include "kai/semantic/SemanticInspectionJson.hpp"

#include <string>
#include <string_view>

namespace kai::semantic {

/// The `file`/`query` fields shared by callers/callees JSON responses -
/// the SAME envelope shape as M2's QueryJsonEnvelope (SemanticQueryJson.hpp)
/// reused here rather than forked, per M3 spec §21.
struct CallQueryJsonEnvelope {
    std::string file;
    InspectionPosition query;
};

/// Serializes a callers/callees query result to:
///
///     {
///       "schemaVersion": 1,
///       "file": "...",
///       "query": {"line":L,"column":C},
///       "function": <SemanticSymbolInfo>|null,
///       "<relationKey>": [
///         { "function": <SemanticSymbolInfo>, "callSites": [<range>, ...] },
///         ...
///       ]
///     }
///
/// `relationKey` is `"callers"` or `"callees"` (a fixed, internally-
/// controlled literal - never escaped/derived from untrusted input).
/// `function` is JSON `null` (M3 spec §6) when the queried position does
/// not resolve to a function - `<relationKey>` is then always `[]`.
/// Reuses appendSymbolJson()/appendEscapedJsonString()/appendPosition()/
/// appendRange() from SemanticInspectionJson.hpp verbatim - the SAME
/// per-symbol/per-range rendering as inspect/definition/references,
/// never a forked shape (M3 spec §21).
std::string writeCallRelationJson(const CallQueryJsonEnvelope& envelope, const CallRelationResult& result,
                                   std::string_view relationKey);

/// Serializes the whole direct call graph to:
///
///     {
///       "schemaVersion": 1,
///       "file": "...",
///       "functions": [
///         { "function": <SemanticSymbolInfo>,
///           "callees": [ { "function": <...>, "callSites": [...] }, ... ] },
///         ...
///       ]
///     }
///
/// `functions` is in source declaration order; a function with no
/// callees still appears, with `"callees": []` (M3 spec §20).
std::string writeCallGraphJson(const std::string& file, const CallGraph& graph);

} // namespace kai::semantic
