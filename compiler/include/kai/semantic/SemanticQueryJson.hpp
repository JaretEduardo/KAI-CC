#pragma once

#include "kai/semantic/SemanticInspectionJson.hpp"
#include "kai/semantic/SemanticQuery.hpp"

#include <string>

namespace kai::semantic {

/// The `file`/`query` fields shared by both definition and references
/// JSON envelopes (M2 spec §10/§11) - `query` echoes back the exact
/// position that was asked about, in the SAME 1-indexed convention as
/// every other position/range this compiler produces.
struct QueryJsonEnvelope {
    std::string file;
    InspectionPosition query;
};

/// Serializes a definition-query result to:
///
///     {
///       "schemaVersion": 1,
///       "file": "...",
///       "query": {"line":L,"column":C},
///       "symbol": <same per-kind shape as M1's inspect symbols> | null
///     }
///
/// `symbol` is JSON `null` (never a fabricated/omitted-but-implied
/// value) when `result` is std::nullopt - M2 spec §17: "no symbol here"
/// is a normal, successful result, not an error. Reuses
/// appendSymbolJson()/appendEscapedJsonString()/appendPosition() from
/// SemanticInspectionJson.hpp verbatim - never a second, subtly
/// different symbol rendering (M2 spec §25). schemaVersion stays 1 (M1's
/// existing inspect schema is unchanged; this is an additive schema,
/// not a breaking one - M2 spec §21).
/// `model` MUST be the SAME SemanticModel `result`'s symbol (if any) was
/// resolved against - see appendSymbolJson()'s own doc comment (KAI
/// LANGUAGE M7A).
std::string writeDefinitionJson(const QueryJsonEnvelope& envelope, const DefinitionResult& result,
                                 const SemanticModel& model);

/// Serializes a references-query result to:
///
///     {
///       "schemaVersion": 1,
///       "file": "...",
///       "query": {"line":L,"column":C},
///       "symbol": <...> | null,
///       "references": [ {"range": {...}}, ... ]
///     }
///
/// `references` is always `[]` when `symbol` is `null`. Array order is
/// exactly `result.references`' own order - SOURCE order, guaranteed by
/// SemanticQuery's own single, source-ordered AST traversal (never
/// re-sorted here).
std::string writeReferencesJson(const QueryJsonEnvelope& envelope, const ReferencesResult& result,
                                 const SemanticModel& model);

} // namespace kai::semantic
