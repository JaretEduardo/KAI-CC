// LLVM CODEGEN MILESTONE 7: the real, end-to-end native compilation
// pipeline - source text on disk -> `kai::cli::runCompileCommand()` (the
// EXACT same orchestration the `kaicc` binary itself runs, never a
// reimplementation) -> a native executable -> actually launching it and
// observing its exit code/stdout. This is the first point in the project
// where a KAI program actually runs.

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/CompileCommand.hpp"
#include "kai/codegen/LLVMCodeGenerator.hpp"
#include "kai/codegen/LLVMObjectEmitter.hpp"
#include "kai/codegen/NativeLinker.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/Module.h>
#include <llvm/Support/Program.h>

#include "support/check.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

using kai::FileId;
using kai::SourceManager;

namespace {

std::filesystem::path writeTempSource(const std::string& baseName, const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / baseName;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return path;
}

// Runs `executablePath` with no arguments, shell-free
// (llvm::sys::ExecuteAndWait - M7 spec §23/§26: a real launch, never a
// mere stat()), capturing its stdout into a temp file and reading it
// back. Returns the process exit code; `capturedStdout` receives
// whatever the process wrote to stdout.
int runAndCaptureStdout(const std::filesystem::path& executablePath, std::string& capturedStdout) {
    const std::filesystem::path stdoutPath = std::filesystem::temp_directory_path() / "kai_e2e_stdout_capture.txt";
    std::error_code ignored;
    std::filesystem::remove(stdoutPath, ignored);

    const std::string executableStr = executablePath.string();
    const std::string stdoutPathStr = stdoutPath.string();
    const std::array<llvm::StringRef, 1> args = {llvm::StringRef(executableStr)};
    const std::array<std::optional<llvm::StringRef>, 3> redirects = {std::nullopt, llvm::StringRef(stdoutPathStr),
                                                                       std::nullopt};

    std::string execError;
    bool execFailed = false;
    const int exitCode = llvm::sys::ExecuteAndWait(executableStr, args, /*Env=*/std::nullopt, redirects,
                                                    /*SecondsToWait=*/10, /*MemoryLimit=*/0, &execError, &execFailed);

    std::ifstream capture(stdoutPath, std::ios::binary);
    std::ostringstream buffer;
    buffer << capture.rdbuf();
    capturedStdout = buffer.str();

    std::filesystem::remove(stdoutPath, ignored);
    return exitCode;
}

struct CompileAndRunResult {
    bool compileSucceeded = false;
    int runExitCode = -1;
    std::string stdoutText;
};

CompileAndRunResult compileAndRun(const std::string& kaiSourceName, const std::string& source) {
    CompileAndRunResult result;

    const std::filesystem::path sourcePath = writeTempSource(kaiSourceName, source);
    const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / (kaiSourceName + ".out");
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);

    SourceManager sm;
    std::ostringstream err;
    const int compileExitCode = kai::cli::runCompileCommand(sm, sourcePath, outputPath, err);
    result.compileSucceeded = compileExitCode == 0;
    if (!result.compileSucceeded) {
        std::cerr << "compile of " << kaiSourceName << " failed: " << err.str();
        std::filesystem::remove(sourcePath, ignored);
        return result;
    }

    result.runExitCode = runAndCaptureStdout(outputPath, result.stdoutText);

    std::filesystem::remove(sourcePath, ignored);
    std::filesystem::remove(outputPath, ignored);
    return result;
}

// REQUIRED E2E (M7 spec §23): print(42)
void testHelloPrint42EndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_hello.kai", "fn main() {\n    print(42)\n}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "42\n");
}

// REQUIRED E2E (M7 spec §24): factorial(5) via recursion + if/else +
// calls + runtime print - proves parser, semantics, integer codegen,
// parameters, calls, recursion, if/else, runtime print, object emission,
// linking, and native execution all compose correctly together.
void testFactorialEndToEnd() {
    const std::string source = "fn factorial(n: i64) -> i64 {\n"
                                "    if n <= 1 {\n"
                                "        return 1\n"
                                "    } else {\n"
                                "        return n * factorial(n - 1)\n"
                                "    }\n"
                                "}\n"
                                "fn main() {\n    print(factorial(5))\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_factorial.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "120\n");
}

// OPTIONAL (M7 spec §25): while-loop native integration.
void testWhileLoopEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    mut x: i64 = 0\n"
                                "\n"
                                "    while x < 3 {\n"
                                "        print(x)\n"
                                "        x = x + 1\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_while.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "0\n1\n2\n");
}

// FRONTEND/BACKEND FAILURE BEHAVIOR (M7 spec §18/§19): no executable is
// ever produced from a failed attempt.

void testCompileFailsCleanlyOnSemanticError() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_bad.kai", "fn main() {\n    print(undefined_var)\n}");
    const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "kai_e2e_bad.out";
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(sm, sourcePath, outputPath, err);

    KAI_CHECK(exitCode != 0);
    KAI_CHECK(!err.str().empty());
    KAI_CHECK(!std::filesystem::exists(outputPath));

    std::filesystem::remove(sourcePath, ignored);
}

void testCompileFailsCleanlyWithNoMain() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_nomain.kai", "fn helper() -> i64 {\n    return 1\n}");
    const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "kai_e2e_nomain.out";
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(sm, sourcePath, outputPath, err);

    KAI_CHECK(exitCode != 0);
    KAI_CHECK(!err.str().empty());
    KAI_CHECK(!std::filesystem::exists(outputPath));

    std::filesystem::remove(sourcePath, ignored);
}

// NATIVE LINKER (M7 spec §22): emit an object, link it with kai_runtime
// via NativeLinker directly, confirm the executable exists AND runs.

void testNativeLinkerProducesRunnableExecutable() {
    // The SAME kaicc/kai_runtime this build just produced (compile
    // definitions - see this test's CMakeLists.txt registration) -
    // exercises NativeLinker::findDefaultRuntimeLibrary()'s real,
    // relocatable, executable-relative discovery against the ACTUAL build
    // output, never a synthetic path.
    const std::filesystem::path kaiccPath(KAI_TEST_KAICC_EXECUTABLE);
    const std::optional<std::filesystem::path> runtimeLibrary =
        kai::codegen::NativeLinker::findDefaultRuntimeLibrary(kaiccPath);
    KAI_CHECK(runtimeLibrary.has_value());
    if (!runtimeLibrary.has_value()) {
        return;
    }
    KAI_CHECK(std::filesystem::exists(*runtimeLibrary));

    const std::optional<std::string> compilerDriver = kai::codegen::NativeLinker::findCompilerDriver();
    KAI_CHECK(compilerDriver.has_value());
    if (!compilerDriver.has_value()) {
        return;
    }

    SourceManager sm;
    const std::filesystem::path sourcePath = writeTempSource("kai_link_test.kai", "fn main() {\n    print(7)\n}");
    const std::filesystem::path objectPath = std::filesystem::temp_directory_path() / "kai_link_test.o";
    const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "kai_link_test.out";
    std::error_code ignored;
    std::filesystem::remove(objectPath, ignored);
    std::filesystem::remove(outputPath, ignored);

    const auto loaded = sm.loadFile(sourcePath);
    KAI_CHECK(loaded.has_value());
    if (!loaded.has_value()) {
        return;
    }

    kai::parser::Parser parser(sm, *loaded);
    auto parsed = parser.parseSourceFile();
    KAI_CHECK(parsed.has_value());
    if (!parsed.has_value()) {
        return;
    }

    kai::semantic::SemanticModel model;
    kai::semantic::SemanticAnalyzer analyzer(sm);
    model = analyzer.analyze(*parsed);
    kai::semantic::TypeChecker checker(sm);
    checker.check(*parsed, model);
    const kai::semantic::ControlFlowAnalyzer flow;
    flow.check(*parsed, model);
    KAI_CHECK(model.errors().empty());

    kai::codegen::LLVMCodeGenerator codegen(sm);
    KAI_CHECK(codegen.generate(*parsed, model));

    llvm::Module& module = codegen.module();
    std::ostringstream adaptErr;
    KAI_CHECK(kai::codegen::LLVMObjectEmitter::adaptNativeEntryPoint(module, adaptErr));

    kai::codegen::LLVMObjectEmitter::initializeNativeTarget();
    std::ostringstream emitErr;
    KAI_CHECK(kai::codegen::LLVMObjectEmitter::emit(module, objectPath, emitErr));
    KAI_CHECK(std::filesystem::exists(objectPath));

    std::ostringstream linkErr;
    KAI_CHECK(kai::codegen::NativeLinker::link(*compilerDriver, objectPath, *runtimeLibrary, outputPath, linkErr));
    KAI_CHECK(std::filesystem::exists(outputPath));

    // Launch it, not merely stat it (M7 spec §26).
    std::string stdoutText;
    const int exitCode = runAndCaptureStdout(outputPath, stdoutText);
    KAI_CHECK(exitCode == 0);
    KAI_CHECK(stdoutText == "7\n");

    std::filesystem::remove(sourcePath, ignored);
    std::filesystem::remove(objectPath, ignored);
    std::filesystem::remove(outputPath, ignored);
}

} // namespace

int main() {
    testHelloPrint42EndToEnd();
    testFactorialEndToEnd();
    testWhileLoopEndToEnd();

    testCompileFailsCleanlyOnSemanticError();
    testCompileFailsCleanlyWithNoMain();

    testNativeLinkerProducesRunnableExecutable();

    return kai::test::failureCount == 0 ? 0 : 1;
}
