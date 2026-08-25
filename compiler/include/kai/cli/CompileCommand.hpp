#pragma once

#include "kai/source/SourceManager.hpp"

#include <filesystem>
#include <ostream>

namespace kai::cli {

/// LLVM CODEGEN MILESTONE 7: the real end-to-end `kaicc <input.kai> -o
/// <output>` pipeline - CLI orchestration only (M7 spec §4/§17), calling
/// into the exact same frontend/backend components every other command
/// (and every codegen test) already uses:
///
///     load source -> Lexer/Parser -> SemanticAnalyzer -> TypeChecker ->
///     ControlFlowAnalyzer -> LLVMCodeGenerator -> native-entry adaptation
///     (LLVMObjectEmitter::adaptNativeEntryPoint) -> LLVMObjectEmitter ->
///     NativeLinker -> executable
///
/// No stage is ever bypassed: a frontend error stops before codegen ever
/// runs; a codegen/emission/link failure never produces or leaves behind
/// an executable at `outputPath` that looks like a success (M7 spec
/// §18/§19). An intermediate `<outputPath>.o` object file is created next
/// to `outputPath` and removed again once this function returns,
/// regardless of outcome (M7 spec §20).
///
/// Returns the process exit code for this command:
///   0  - `outputPath` now exists and is the requested native executable
///   2  - the source file failed to load
///   4  - the source failed to parse
///   5  - semantic analysis/type checking/control-flow checking reported
///        at least one error
///   6  - LLVM IR generation failed
///   7  - native-entry adaptation failed (no/unsupported `main` shape)
///   8  - native object emission failed
///   9  - no usable host C compiler driver was found
///   10 - locating the default KAI runtime library failed
///   11 - linking the final executable failed
int runCompileCommand(SourceManager& sources, const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath, std::ostream& err);

} // namespace kai::cli
