#pragma once

#include "kai/source/SourceManager.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>

namespace kai::cli {

/// SEMANTIC INSPECTION MILESTONE 2: which query `runSemanticQueryCommand`
/// performs - `definition`/`references` share the exact same frontend
/// pipeline and failure policy, differing only in which SemanticQuery
/// method (and JSON writer) runs at the end, so one shared command
/// function serves both CLI subcommands (M2 spec §34).
enum class SemanticQueryKind : std::uint8_t {
    Definition,
    References,
};

/// `kaicc definition <file.kai> --line N --column M --json` /
/// `kaicc references <file.kai> --line N --column M --json` - CLI
/// orchestration only, mirroring CompileCommand.hpp/InspectCommand.hpp's
/// own architecture. Runs the REAL frontend:
///
///     load source -> Lexer/Parser -> SemanticAnalyzer -> TypeChecker ->
///     ControlFlowAnalyzer -> SemanticQuery -> JSON
///
/// and NEVER runs LLVM codegen/object emission/linking. `line`/`column`
/// are already-validated positive 1-indexed values (see main.cpp's own
/// argv parsing) - this function performs no further argument validation
/// itself. A syntactically valid but out-of-source-range position is a
/// normal, successful query (M2 spec §19): it simply resolves no
/// occurrence, producing `"symbol": null` (and `"references": []` for a
/// references query) with exit code 0 - never a command-line error.
///
/// stdout/stderr discipline (M1 spec §26/§27, unchanged for M2): on
/// success, `out` receives EXACTLY the JSON text followed by one
/// trailing newline; on any failure, `out` stays untouched and `err`
/// receives a concise diagnostic instead.
///
/// M2 policy (spec §20, same as M1's inspect): if the frontend reports
/// ANY error (a load failure, a parse error, or any SemanticAnalyzer/
/// TypeChecker/ControlFlowAnalyzer error), NO JSON is emitted - there is
/// no partial-query mode.
///
/// Returns the process exit code:
///   0 - JSON was written to `out` (including a successful "no symbol
///       found" result)
///   2 - the source file failed to load
///   4 - the source failed to parse
///   5 - semantic analysis/type checking/control-flow checking reported
///       at least one error
int runSemanticQueryCommand(SemanticQueryKind kind, SourceManager& sources, const std::filesystem::path& inputPath,
                             std::uint32_t line, std::uint32_t column, std::ostream& out, std::ostream& err);

} // namespace kai::cli
