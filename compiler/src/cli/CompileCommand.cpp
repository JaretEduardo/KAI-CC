#include "kai/cli/CompileCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/SemanticErrorFormat.hpp"
#include "kai/codegen/LLVMCodeGenerator.hpp"
#include "kai/codegen/LLVMObjectEmitter.hpp"
#include "kai/codegen/NativeLinker.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"

#include <llvm/IR/Module.h>

#include <cctype>
#include <optional>
#include <string>
#include <system_error>

namespace kai::cli {

#ifdef _WIN32
namespace {

// Case-insensitive ".exe" check - std::filesystem::path::extension()
// itself does not fold case, and a user/build-script-provided `-o
// HELLO.EXE` is still an already-suffixed path that must not be
// double-suffixed. Compiled only on Windows - resolveNativeExecutablePath()
// is the only caller, and it is a no-op on every other platform (see its
// own doc comment in CompileCommand.hpp).
bool hasExeExtensionCaseInsensitive(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension.size() != 4) {
        return false;
    }
    return std::tolower(static_cast<unsigned char>(extension[0])) == '.' &&
           std::tolower(static_cast<unsigned char>(extension[1])) == 'e' &&
           std::tolower(static_cast<unsigned char>(extension[2])) == 'x' &&
           std::tolower(static_cast<unsigned char>(extension[3])) == 'e';
}

} // namespace
#endif

std::filesystem::path resolveNativeExecutablePath(const std::filesystem::path& requestedOutputPath) {
#ifdef _WIN32
    if (hasExeExtensionCaseInsensitive(requestedOutputPath)) {
        return requestedOutputPath;
    }
    std::filesystem::path suffixed = requestedOutputPath;
    suffixed += ".exe";
    return suffixed;
#else
    return requestedOutputPath;
#endif
}

int runCompileCommand(SourceManager& sources, const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath, std::ostream& err) {
    const auto loaded = sources.loadFile(inputPath);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << inputPath.string() << "': " << loaded.error().message() << '\n';
        return 2;
    }

    parser::Parser parser(sources, *loaded);
    const auto parsed = parser.parseSourceFile();
    if (!parsed) {
        err << formatParseError(sources, parsed.error()) << '\n';
        return 4;
    }

    semantic::SemanticModel model;
    semantic::SemanticAnalyzer analyzer(sources);
    model = analyzer.analyze(*parsed);

    semantic::TypeChecker checker(sources);
    checker.check(*parsed, model);

    const semantic::ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    if (!model.errors().empty()) {
        for (const semantic::SemanticError& error : model.errors()) {
            err << formatSemanticError(sources, error, model) << '\n';
        }
        return 5;
    }

    codegen::LLVMCodeGenerator generator(sources);
    if (!generator.generate(*parsed, model)) {
        // RELEASE HARDENING M2: prefer a specific, actionable message when
        // generate() itself identified WHICH explicitly-deferred AST
        // construct it rejected (see LLVMCodeGenerator::
        // unsupportedConstruct()'s own doc comment) - this is never set
        // for a genuine llvm::verifyModule() failure or other internal
        // issue, so the generic message below still covers those
        // honestly, rather than every codegen failure guessing "probably
        // unsupported".
        if (const auto& unsupported = generator.unsupportedConstruct(); unsupported.has_value()) {
            const SourceManager::LineColumn where = sources.lineColumn(unsupported->span.begin());
            err << "kaicc: error at " << where.line << ':' << where.column << ": " << unsupported->description
                << '\n';
        } else {
            err << "kaicc: error: LLVM IR generation failed\n";
        }
        return 6;
    }

    llvm::Module& module = generator.module();

    if (!codegen::LLVMObjectEmitter::adaptNativeEntryPoint(module, err)) {
        return 7;
    }

    codegen::LLVMObjectEmitter::initializeNativeTarget();

    // WINDOWS M1 spec §7: the actual final linked-executable path - a
    // no-op on every platform except Windows, where a missing `.exe`
    // extension is filled in (see resolveNativeExecutablePath()'s own
    // doc comment). The intermediate object file is still named from the
    // ORIGINAL requested `outputPath` - it is a temporary, always removed
    // before this function returns, so its exact name is not
    // user-visible and does not need the same treatment.
    const std::filesystem::path nativeOutputPath = resolveNativeExecutablePath(outputPath);

    std::filesystem::path objectPath = outputPath;
    objectPath += ".o";

    // Best-effort cleanup of the intermediate object file on every exit
    // path below (M7 spec §20) - no temp-file framework, just removing
    // whatever this function itself created.
    const auto cleanupObject = [&objectPath]() {
        std::error_code ignored;
        std::filesystem::remove(objectPath, ignored);
    };

    if (!codegen::LLVMObjectEmitter::emit(module, objectPath, err)) {
        cleanupObject();
        return 8;
    }

    const std::optional<std::string> compilerDriver = codegen::NativeLinker::findCompilerDriver();
    if (!compilerDriver.has_value()) {
        err << "kaicc: error: no usable host C compiler driver found (tried $KAI_CC, cc, clang, gcc)\n";
        cleanupObject();
        return 9;
    }

    const std::filesystem::path kaiccPath = codegen::NativeLinker::currentExecutablePath();
    const std::optional<std::filesystem::path> runtimeLibrary =
        codegen::NativeLinker::findDefaultRuntimeLibrary(kaiccPath);
    if (!runtimeLibrary.has_value()) {
        err << "kaicc: error: could not locate the default KAI runtime library (libkai_runtime.a) - set "
               "KAI_RUNTIME_LIB to override\n";
        cleanupObject();
        return 10;
    }

    if (!codegen::NativeLinker::link(*compilerDriver, objectPath, *runtimeLibrary, nativeOutputPath, err)) {
        std::error_code ignored;
        std::filesystem::remove(nativeOutputPath, ignored); // no partial/stale executable left behind
        cleanupObject();
        return 11;
    }

    cleanupObject();
    return 0;
}

} // namespace kai::cli
