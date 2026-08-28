#pragma once

#include "kai/semantic/SemanticInspector.hpp"

#include <string>
#include <string_view>

namespace kai::semantic {

/// A small, reusable JSON-string escape helper (M1 spec §19: do not
/// hand-concatenate unsafe/unescaped JSON strings; M2 spec §7: reuse this
/// primitive rather than re-implementing a second escaper). Escapes
/// exactly what the JSON spec requires: quote, backslash, and every
/// control character (0x00-0x1F) - either via a named two-character
/// escape where JSON defines one, or a `\u00XX` escape otherwise. Every
/// other byte passes through unchanged. Appends the finished, quoted
/// JSON string literal (including both surrounding `"` characters) onto
/// `out`.
void appendEscapedJsonString(std::string& out, std::string_view text);

/// Appends `{"line":L,"column":C}` for `position` onto `out`.
void appendPosition(std::string& out, const InspectionPosition& position);

/// Appends `{"start":{...},"end":{...}}` for `range` onto `out` - the
/// ONE canonical rendering of an InspectionRange, reused by every JSON
/// shape this compiler produces (M1's `definition` fields, M2's
/// `references[].range`).
void appendRange(std::string& out, const InspectionRange& range);

/// Appends the exact per-kind JSON object M1's `writeSemanticInspectionJson()`
/// itself uses for one symbol (Function: `parameters`+`returnType`;
/// Parameter/Local: `type`+`enclosingFunction`) onto `out` - reused
/// as-is by M2's definition/references JSON so a symbol never has two
/// subtly different JSON shapes depending on which command produced it
/// (M2 spec §25). `model` MUST be the SAME SemanticModel `symbol`'s own
/// Type field(s) were resolved against (KAI LANGUAGE M7A: needed to
/// render a fixed-size array type as "[i32; 3]" via semantic::typeName()
/// - see that function's own doc comment for the exact lifetime
/// contract this establishes).
void appendSymbolJson(std::string& out, const SemanticSymbolInfo& symbol, const SemanticModel& model);

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
///
/// `model` MUST be the SAME SemanticModel `result` was produced from
/// (every call site already has it in scope - see appendSymbolJson()'s
/// own doc comment for why KAI LANGUAGE M7A needs it).
std::string writeSemanticInspectionJson(const SemanticInspectionResult& result, const SemanticModel& model);

} // namespace kai::semantic
