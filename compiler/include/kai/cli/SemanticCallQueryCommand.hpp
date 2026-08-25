#pragma once

#include "kai/source/SourceManager.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>

namespace kai::cli {

/// SEMANTIC INSPECTION MILESTONE 3: which relation `runCallQueryCommand`
/// reports - `callers`/`callees` share the exact same frontend pipeline
/// and failure policy, differing only in which SemanticCallQuery method
/// (and JSON relation key) runs at the end.
enum class CallQueryKind : std::uint8_t {
    Callers,
    Callees,
};

/// `kaicc callers <file.kai> --line N --column M --json` /
/// `kaicc callees <file.kai> --line N --column M --json` - CLI
/// orchestration only, mirroring CompileCommand/InspectCommand/
/// SemanticQueryCommand's own architecture. Runs the REAL frontend (no
/// LLVM). `line`/`column` are already-validated positive 1-indexed
/// values (see main.cpp's own argv parsing).
///
/// A position that resolves to something other than a function (a
/// Local/Parameter, or nothing at all) is a normal, successful query
/// (M3 spec §6): `"function": null` and an empty relation array, exit 0
/// - never a command-line error.
///
/// M3 policy (same as M1/M2): if the frontend reports ANY error, NO JSON
/// is emitted - stdout stays empty, stderr gets the diagnostic, exit
/// nonzero.
///
/// Returns the process exit code:
///   0 - JSON was written to `out` (including a successful
///       "no function found" result)
///   2 - the source file failed to load
///   4 - the source failed to parse
///   5 - semantic analysis/type checking/control-flow checking reported
///       at least one error
int runCallQueryCommand(CallQueryKind kind, SourceManager& sources, const std::filesystem::path& inputPath,
                         std::uint32_t line, std::uint32_t column, std::ostream& out, std::ostream& err);

/// `kaicc call-graph <file.kai> --json` - the whole direct call graph of
/// the file, same frontend pipeline/failure policy as above but with no
/// position to validate.
///
/// Returns the process exit code:
///   0 - JSON was written to `out`
///   2 - the source file failed to load
///   4 - the source failed to parse
///   5 - semantic analysis/type checking/control-flow checking reported
///       at least one error
int runCallGraphCommand(SourceManager& sources, const std::filesystem::path& inputPath, std::ostream& out,
                         std::ostream& err);

} // namespace kai::cli
