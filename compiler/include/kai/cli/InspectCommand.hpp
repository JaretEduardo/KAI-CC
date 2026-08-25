#pragma once

#include "kai/source/SourceManager.hpp"

#include <filesystem>
#include <ostream>

namespace kai::cli {

/// SEMANTIC INSPECTION MILESTONE 1: `kaicc inspect <input.kai> --json` -
/// CLI orchestration only, mirroring CompileCommand.hpp's own
/// architecture. Runs the REAL frontend:
///
///     load source -> Lexer/Parser -> SemanticAnalyzer -> TypeChecker ->
///     ControlFlowAnalyzer -> SemanticInspector -> JSON
///
/// and NEVER runs LLVM codegen/object emission/linking - inspection is a
/// purely frontend + semantic-query operation (M1 spec §16). This
/// function owns no semantic-traversal or JSON-serialization logic of
/// its own - see kai::semantic::SemanticInspector /
/// kai::semantic::writeSemanticInspectionJson().
///
/// stdout/stderr discipline (M1 spec §26/§27): on success, `out` receives
/// EXACTLY the JSON text followed by one trailing newline and nothing
/// else (no "Inspecting file..." chatter); on any failure, `out` stays
/// completely untouched, and `err` receives a concise diagnostic instead.
/// A caller can therefore trust "exit code 0" to mean "valid JSON was
/// written to `out`", never a partial or mixed result.
///
/// M1 policy (spec §25): if the frontend reports ANY error (a load
/// failure, a parse error, or any SemanticAnalyzer/TypeChecker/
/// ControlFlowAnalyzer error), NO JSON is emitted at all - there is no
/// partial-inspection mode yet.
///
/// Returns the process exit code:
///   0 - JSON was written to `out`
///   2 - the source file failed to load
///   4 - the source failed to parse
///   5 - semantic analysis/type checking/control-flow checking reported
///       at least one error
int runInspectCommand(SourceManager& sources, const std::filesystem::path& inputPath, std::ostream& out,
                       std::ostream& err);

} // namespace kai::cli
