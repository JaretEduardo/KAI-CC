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

// RELEASE HARDENING M2.1: SemanticErrorFormat.cpp's TypeMismatch detail
// (M2) - tested through the real CLI/CompileCommand layer (runCompileCommand()),
// never merely the SemanticErrorKind enum, so a regression in the actual
// formatter/output layer would be caught.
void testTypeMismatchDiagnosticIncludesExpectedAndActualTypes() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_type_mismatch.kai", "fn main() {\n    let x: str = 42\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode =
        kai::cli::runCompileCommand(sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_tm.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("type mismatch: expected str, got i32") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// RELEASE HARDENING M2.1: SemanticErrorFormat.cpp's LiteralOutOfRange
// detail (M2).
void testLiteralOutOfRangeDiagnosticIncludesTargetType() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_literal_range.kai", "fn main() {\n    let x: i8 = 200\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode =
        kai::cli::runCompileCommand(sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_lor.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("literal out of range: does not fit in i8") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// RELEASE HARDENING M2.1: the `for` statement must produce the specific,
// actionable message CompileCommand.cpp now surfaces from
// LLVMCodeGenerator::unsupportedConstruct() (M2) - REQUIRED regression:
// proves the generic "LLVM IR generation failed" text is no longer what a
// user sees for this exact, already-identified-as-deferred construct.
void testUnsupportedForStatementDiagnosticIsActionable() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_unsupported_for.kai", "fn main() {\n    for i in 0..3 {\n        print(i)\n    }\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode =
        kai::cli::runCompileCommand(sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_for.out", err);

    KAI_CHECK(exitCode == 6);
    KAI_CHECK(err.str().find("code generation is not yet supported for 'for' statements") != std::string::npos);
    KAI_CHECK(err.str().find("LLVM IR generation failed") == std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// RELEASE HARDENING M2.1: an unsupported (array/slice) parameter type
// must ALSO get its own specific message (M2's second
// recordUnsupportedConstruct() call site, in declareFunction()'s
// parameter-type loop) - not the generic fallback.
void testUnsupportedParameterTypeDiagnosticIsActionable() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_unsupported_param.kai",
        "fn sum(values: [i32]) -> i32 {\n    return 0\n}\nfn main() {\n    print(0)\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_param.out", err);

    KAI_CHECK(exitCode == 6);
    KAI_CHECK(err.str().find("code generation is not yet supported for this parameter's type") != std::string::npos);

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

// MINIMAL STRING LITERAL SUPPORT (M8 spec §21): the exact required user-
// visible capability - print("Hello, KAI!") end-to-end.
void testPrintStringLiteralEndToEnd() {
    const CompileAndRunResult result =
        compileAndRun("kai_e2e_string_hello.kai", "fn main() {\n    print(\"Hello, KAI!\")\n}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "Hello, KAI!\n");
}

// M8 REQUIRED consistency case (spec §1): once a string literal has a
// concrete Type, `let message = "..."` then `print(message)` must also
// work end-to-end through the real alloca/store/load local machinery -
// no string-specific storage path was added.
void testPrintInferredStringLocalEndToEnd() {
    const std::string source = "fn main() {\n    let message = \"Welcome to KAI\"\n    print(message)\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_string_local.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "Welcome to KAI\n");
}

// M8 REQUIRED (spec §15): escape decoding - \n \" \\ \t all produce their
// actual decoded bytes in the printed output, never the source backslash
// sequence. Raw string literals (R"KAI(...)KAI") are used here so the
// KAI source text's own backslashes appear literally, with no C++-layer
// double-escaping to keep track of.
void testStringEscapeDecodingEndToEnd() {
    const std::string source = R"KAI(fn main() {
    print("a\nb")
    print("quote: \"")
    print("slash: \\")
    print("tab:\t")
}
)KAI";
    const CompileAndRunResult result = compileAndRun("kai_e2e_string_escapes.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);

    const std::string expected = std::string("a\nb\n") + "quote: \"\n" + "slash: \\\n" + "tab:\t\n";
    KAI_CHECK(result.stdoutText == expected);
}

// M8 REQUIRED regression (spec §14): print("a\0b") must produce the EXACT
// bytes 61 00 62 0a, never truncated at the embedded NUL. This comparison
// is byte-exact, never a C-string/strlen-based one: `stdoutText` comes
// from runAndCaptureStdout() reading the captured file through
// std::ifstream/std::ostringstream, so an embedded '\0' survives intact
// in the std::string, and `== std::string("a\0b\n", 4)` compares all 4
// bytes rather than stopping at the '\0'.
void testEmbeddedNulExactBytesEndToEnd() {
    const std::string source = R"KAI(fn main() {
    print("a\0b")
}
)KAI";
    const CompileAndRunResult result = compileAndRun("kai_e2e_string_nul.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText.size() == 4);
    KAI_CHECK(result.stdoutText == std::string("a\0b\n", 4));
}

// M8 REQUIRED (spec §16): a non-ASCII UTF-8 literal prints its exact
// bytes. This is a byte-preservation proof only - no Unicode
// normalization or codepoint iteration is implemented or implied. `✓`
// (CHECK MARK) is used in both the KAI source and the expected value so
// both sides go through the same compiler UTF-8 encoding, independent of
// this source file's own on-disk encoding.
void testUtf8ExactBytesEndToEnd() {
    const std::string source = "fn main() {\n    print(\"KAI ✓\")\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_string_utf8.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "KAI ✓\n");
}

// SPELLABLE STR + PARAMETERS/RETURNS MVP (M9 spec §27): a str parameter.
void testStrParameterEndToEnd() {
    const std::string source = "fn greet(name: str) {\n    print(name)\n}\nfn main() {\n    greet(\"Hello, KAI!\")\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_param.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "Hello, KAI!\n");
}

// M9 spec §27: a function returning a static str literal.
void testStrReturnEndToEnd() {
    const std::string source = "fn language() -> str {\n    return \"KAI\"\n}\nfn main() {\n    print(language())\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_return.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "KAI\n");
}

// M9 spec §27: single-str-parameter passthrough return.
void testStrEchoEndToEnd() {
    const std::string source =
        "fn echo(value: str) -> str {\n    return value\n}\nfn main() {\n    print(echo(\"hello\"))\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_echo.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "hello\n");
}

// M9 spec §27: explicit `str` local annotation.
void testExplicitStrLocalEndToEnd() {
    const std::string source = "fn main() {\n    let message: str = \"typed\"\n    print(message)\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_explicit_local.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "typed\n");
}

// M9 spec §27: forwarding a str parameter through at least two functions.
void testStrForwardingThroughTwoFunctionsEndToEnd() {
    const std::string source = "fn inner(value: str) {\n    print(value)\n}\n"
                                "fn outer(value: str) {\n    inner(value)\n}\n"
                                "fn main() {\n    outer(\"forwarded\")\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_forwarding.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "forwarded\n");
}

// M9 REQUIRED regression (spec §27.6): embedded NUL through a function
// boundary (parameter -> return -> print) must preserve exact bytes -
// never truncated by a strlen-based path anywhere along the call chain.
void testStrEmbeddedNulThroughFunctionBoundaryEndToEnd() {
    const std::string source = R"KAI(fn echo(value: str) -> str {
    return value
}

fn main() {
    print(echo("a\0b"))
}
)KAI";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_nul_boundary.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText.size() == 4);
    KAI_CHECK(result.stdoutText == std::string("a\0b\n", 4));
}

// M9 REQUIRED regression (spec §27.7): UTF-8 bytes through a function
// boundary must be preserved exactly.
void testStrUtf8ThroughFunctionBoundaryEndToEnd() {
    const std::string source = "fn echo(value: str) -> str {\n    return value\n}\n"
                                "fn main() {\n    print(echo(\"KAI ✓\"))\n}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_str_utf8_boundary.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK(result.stdoutText == "KAI ✓\n");
}

} // namespace

int main() {
    testHelloPrint42EndToEnd();
    testFactorialEndToEnd();
    testWhileLoopEndToEnd();

    testCompileFailsCleanlyOnSemanticError();
    testCompileFailsCleanlyWithNoMain();

    testTypeMismatchDiagnosticIncludesExpectedAndActualTypes();
    testLiteralOutOfRangeDiagnosticIncludesTargetType();
    testUnsupportedForStatementDiagnosticIsActionable();
    testUnsupportedParameterTypeDiagnosticIsActionable();

    testNativeLinkerProducesRunnableExecutable();

    testPrintStringLiteralEndToEnd();
    testPrintInferredStringLocalEndToEnd();
    testStringEscapeDecodingEndToEnd();
    testEmbeddedNulExactBytesEndToEnd();
    testUtf8ExactBytesEndToEnd();

    testStrParameterEndToEnd();
    testStrReturnEndToEnd();
    testStrEchoEndToEnd();
    testExplicitStrLocalEndToEnd();
    testStrForwardingThroughTwoFunctionsEndToEnd();
    testStrEmbeddedNulThroughFunctionBoundaryEndToEnd();
    testStrUtf8ThroughFunctionBoundaryEndToEnd();

    return kai::test::failureCount == 0 ? 0 : 1;
}
