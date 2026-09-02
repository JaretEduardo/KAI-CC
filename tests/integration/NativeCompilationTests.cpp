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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

using kai::FileId;
using kai::SourceManager;

namespace {

// WINDOWS M1.1: a captured stdout mismatch used to report only the two
// std::string values via KAI_CHECK's own `#expr` text - unreadable for a
// byte-level difference like Windows CRT text-mode LF -> CRLF
// translation (every byte printable ASCII except the exact byte that
// differs). Prints each byte as its literal character when printable
// ASCII, otherwise as `\xHH` - deliberately not a raw hex dump (these
// captures are always a handful of bytes - M1.1 spec §9 "avoid dumping
// enormous buffers" - and an escaped form makes the CRLF/NUL story
// visible at a glance, e.g. "42\x0d\x0a" immediately reads as a CR
// before the expected LF, or "a\x00b\x0a" immediately reads as the
// embedded NUL surviving intact).
std::string escapeBytesForDiagnostic(const std::string& bytes) {
    std::ostringstream out;
    for (const unsigned char byte : bytes) {
        if (byte >= 0x20 && byte < 0x7F) {
            out << static_cast<char>(byte);
        } else {
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte)
                << std::dec;
        }
    }
    return out.str();
}

// Reports a captured-stdout mismatch with both byte counts and the
// escaped byte content of each side (M1.1 spec §1/§9) before delegating
// to the same failure-counting mechanism every other KAI_CHECK uses -
// never a second, separate pass/fail bookkeeping scheme.
void checkStdoutBytes(const std::string& actual, const std::string& expected, const char* exprText, const char* file,
                      int line) {
    if (actual == expected) {
        return;
    }
    kai::test::reportFailure(exprText, file, line);
    std::cerr << "    expected (" << expected.size() << " bytes): \"" << escapeBytesForDiagnostic(expected)
               << "\"\n";
    std::cerr << "    actual   (" << actual.size() << " bytes): \"" << escapeBytesForDiagnostic(actual) << "\"\n";
}

// Drop-in replacement for `KAI_CHECK(actual == expected)` specifically
// for captured-stdout comparisons - same pass/fail semantics and exit
// code, plus the escaped byte dump above on failure. Every other
// KAI_CHECK in this file (exit codes, byte counts, boolean results)
// is unaffected.
#define KAI_CHECK_STDOUT_BYTES(actual, expected)                                                                    \
    checkStdoutBytes((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

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
    // WINDOWS M1 spec §7: the file runCompileCommand() actually produces -
    // on every platform except Windows this is `outputPath` itself
    // unchanged; on Windows a missing `.exe` is filled in. Every caller
    // that needs the real produced filename uses this exact function
    // (kai::cli::resolveNativeExecutablePath()'s own doc comment) rather
    // than assuming `outputPath` is the literal produced file.
    const std::filesystem::path nativeOutputPath = kai::cli::resolveNativeExecutablePath(outputPath);
    std::error_code ignored;
    std::filesystem::remove(nativeOutputPath, ignored);

    SourceManager sm;
    std::ostringstream err;
    const int compileExitCode = kai::cli::runCompileCommand(sm, sourcePath, outputPath, err);
    result.compileSucceeded = compileExitCode == 0;
    if (!result.compileSucceeded) {
        std::cerr << "compile of " << kaiSourceName << " failed: " << err.str();
        std::filesystem::remove(sourcePath, ignored);
        return result;
    }

    result.runExitCode = runAndCaptureStdout(nativeOutputPath, result.stdoutText);

    std::filesystem::remove(sourcePath, ignored);
    std::filesystem::remove(nativeOutputPath, ignored);
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "42\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "120\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n1\n2\n");
}

// --- KAI LANGUAGE M6 (post-alpha.2): `for` + integer ranges, real
// native execution ---

// A. REQUIRED: `for i in 0..3 { print(i) }` -> "0\n1\n2\n" exactly.
void testForLoopLiteralRangeEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    for i in 0..3 {\n"
                                "        print(i)\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_literal.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n1\n2\n");
}

// B. REQUIRED: `start == end` -> zero iterations, empty stdout.
void testForLoopZeroIterationsWhenStartEqualsEndEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    for i in 3..3 {\n"
                                "        print(i)\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_zero.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "");
}

// C. REQUIRED: `start > end` -> zero iterations, empty stdout - no
// wrapping/reverse iteration.
void testForLoopZeroIterationsWhenStartExceedsEndEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    for i in 5..2 {\n"
                                "        print(i)\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_descending.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "");
}

// D. REQUIRED: range endpoints from variables (not just literals).
void testForLoopRangeFromVariablesEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let start: i32 = 1\n"
                                "    let stop: i32 = 4\n"
                                "    for i in start..stop {\n"
                                "        print(i)\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_variables.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n2\n3\n");
}

// E. REQUIRED: nested `for` loops.
void testForLoopNestedEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    for i in 0..2 {\n"
                                "        for j in 0..2 {\n"
                                "            print(i)\n"
                                "            print(j)\n"
                                "        }\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_nested.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n0\n0\n1\n1\n0\n1\n1\n");
}

// F. REQUIRED: `return` from inside a `for` body exits the enclosing
// function immediately, never completing the remaining iterations.
void testForLoopReturnInsideBodyEndToEnd() {
    const std::string source = "fn find() -> i32 {\n"
                                "    for i in 0..10 {\n"
                                "        if i == 3 {\n"
                                "            return i\n"
                                "        }\n"
                                "        print(i)\n"
                                "    }\n"
                                "    return -1\n"
                                "}\n"
                                "fn main() {\n"
                                "    print(find())\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_return.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    // i=0,1,2 print before the i==3 return fires; iterations 3-9 never run.
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n1\n2\n3\n");
}

// G. REQUIRED: an outer binding with the same name is shadowed inside
// the loop body and is UNCHANGED after the loop ends.
void testForLoopOuterVariableShadowingEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let i: i32 = 99\n"
                                "    for i in 0..3 {\n"
                                "        print(i)\n"
                                "    }\n"
                                "    print(i)\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_shadow.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n1\n2\n99\n");
}

// M6 spec #12: an OBSERVABLE regression test proving `start`/`end` are
// evaluated EXACTLY ONCE before the loop, never once per iteration -
// each endpoint is a function call with a visible print side effect, so
// re-evaluation would print "100"/"200" more than once.
void testForLoopEndpointsEvaluatedOnceEndToEnd() {
    const std::string source = "fn start() -> i32 {\n"
                                "    print(100)\n"
                                "    return 0\n"
                                "}\n"
                                "\n"
                                "fn end() -> i32 {\n"
                                "    print(200)\n"
                                "    return 3\n"
                                "}\n"
                                "\n"
                                "fn main() {\n"
                                "    for i in start()..end() {\n"
                                "        print(i)\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_for_endpoints_once.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "100\n200\n0\n1\n2\n");
}

// --- KAI LANGUAGE M7B (post-alpha.2): local fixed-size arrays + checked
// indexing, real native execution ---

// A. REQUIRED: basic read.
void testArrayLiteralIndexedReadEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    print(xs[0])\n"
                                "    print(xs[2])\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_read.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "10\n30\n");
}

// B. REQUIRED: mutable element write.
void testArrayIndexedWriteEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    mut xs = [10, 20, 30]\n"
                                "    xs[1] = 99\n"
                                "    print(xs[1])\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_write.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "99\n");
}

// C. REQUIRED: M6 for-range read integration.
void testArrayM6ForRangeReadEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    for i in 0..3 {\n"
                                "        print(xs[i])\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_for_read.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "10\n20\n30\n");
}

// D. REQUIRED: M6 for-range mutation integration.
void testArrayM6ForRangeMutationEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    mut xs = [10, 20, 30]\n"
                                "    for i in 0..3 {\n"
                                "        xs[i] = xs[i] + 1\n"
                                "    }\n"
                                "    for i in 0..3 {\n"
                                "        print(xs[i])\n"
                                "    }\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_for_mutate.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "11\n21\n31\n");
}

// E. REQUIRED: array literal evaluation order / exactly once.
void testArrayLiteralEvaluationOrderEndToEnd() {
    const std::string source = "fn a() -> i32 {\n"
                                "    print(100)\n"
                                "    return 1\n"
                                "}\n"
                                "fn b() -> i32 {\n"
                                "    print(200)\n"
                                "    return 2\n"
                                "}\n"
                                "fn main() {\n"
                                "    let xs = [a(), b()]\n"
                                "    print(xs[0])\n"
                                "    print(xs[1])\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_eval_order.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "100\n200\n1\n2\n");
}

// F. REQUIRED: dynamic in-bounds signed index.
void testArrayDynamicSignedInBoundsEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    mut i: i32 = 1\n"
                                "    print(xs[i])\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_dyn_signed_ok.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "20\n");
}

// G. REQUIRED: dynamic out-of-bounds upper-bound index traps. Per M7B
// spec §15/§18: no stable OS exit code is asserted for llvm.trap - only
// that the process does NOT report successful completion, and that no
// output past the trapping access ever appears (a fixed marker BEFORE
// the trapping print proves the program made it that far and no
// further - see this test's own "999" sentinel and its absence check,
// not asserted as surviving the abrupt termination itself, only as the
// LAST thing that could possibly appear).
void testArrayDynamicSignedUpperBoundTrapsEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    mut i: i32 = 3\n"
                                "    print(xs[i])\n"
                                "    print(888)\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_dyn_signed_oob.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    // The statement AFTER the trapping index is provably unreachable
    // (the trap block ends in `unreachable`, never falling through) -
    // its own output must never appear, regardless of stdio buffering
    // around the abrupt termination itself.
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// H. REQUIRED: dynamic negative signed index traps.
void testArrayDynamicSignedNegativeTrapsEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    mut i: i32 = -1\n"
                                "    print(xs[i])\n"
                                "    print(888)\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_dyn_signed_negative.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// I. REQUIRED: dynamic unsigned in-bounds succeeds, out-of-bounds traps.
void testArrayDynamicUnsignedInBoundsEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    mut i: u32 = 2\n"
                                "    print(xs[i])\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_dyn_unsigned_ok.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "30\n");
}

void testArrayDynamicUnsignedOutOfBoundsTrapsEndToEnd() {
    const std::string source = "fn main() {\n"
                                "    let xs = [10, 20, 30]\n"
                                "    mut i: u32 = 3\n"
                                "    print(xs[i])\n"
                                "    print(888)\n"
                                "}";
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_dyn_unsigned_oob.kai", source);

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// J. REQUIRED: immutable indexed assignment fails in the FRONTEND
// (exit 5, a real SemanticError) - never a backend/LLVM crash.
void testArrayImmutableIndexedAssignmentFailsInFrontend() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_array_immutable_write.kai",
                         "fn main() {\n    let xs = [1, 2, 3]\n    xs[0] = 5\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_array_immutable.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("assignment to immutable binding") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// Compile-time out-of-bounds diagnostics (M7B spec §15/§22): rejected
// during semantic/type checking (exit 5), NEVER an LLVM/backend error.
void testArrayCompileTimeOutOfBoundsDiagnostics() {
    struct Case {
        const char* index;
        const char* label;
    };
    const Case cases[] = {
        {"3", "kai_e2e_array_oob_upper.kai"},
        {"999", "kai_e2e_array_oob_far.kai"},
        {"-1", "kai_e2e_array_oob_negative.kai"},
    };

    for (const Case& testCase : cases) {
        const std::filesystem::path sourcePath = writeTempSource(
            testCase.label,
            std::string("fn main() {\n    let xs = [1, 2, 3]\n    print(xs[") + testCase.index + "])\n}");
        std::error_code ignored;

        SourceManager sm;
        std::ostringstream err;
        const int exitCode = kai::cli::runCompileCommand(
            sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_array_oob.out", err);

        KAI_CHECK(exitCode == 5);
        KAI_CHECK(err.str().find("array index out of bounds") != std::string::npos);

        std::filesystem::remove(sourcePath, ignored);
    }

    // The valid boundary (length - 1) must NOT be rejected.
    const std::filesystem::path validPath =
        writeTempSource("kai_e2e_array_oob_boundary_valid.kai", "fn main() {\n    let xs = [1, 2, 3]\n    print(xs[2])\n}");
    const std::filesystem::path validOutputPath = std::filesystem::temp_directory_path() / "kai_e2e_array_oob_valid.out";
    std::error_code ignored;
    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(sm, validPath, validOutputPath, err);
    KAI_CHECK(exitCode == 0);
    std::filesystem::remove(validPath, ignored);
    std::filesystem::remove(kai::cli::resolveNativeExecutablePath(validOutputPath), ignored);
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

// RETARGETED (KAI LANGUAGE M6, post-alpha.2): a `for` statement over a
// literal integer range now compiles and runs (see the M6 end-to-end
// test group below) - the diagnostic-actionability coverage this test
// originally proved now belongs to the one iterable shape `for` still
// rejects: anything that isn't a literal `start..end` range. TypeChecker
// rejects this directly (SemanticErrorKind::UnsupportedForIterable,
// exit code 5 - a semantic error, never reaching codegen at all), so the
// message is now a semantic diagnostic rather than
// LLVMCodeGenerator::unsupportedConstruct()'s own text.
void testUnsupportedForIterableDiagnosticIsActionable() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_unsupported_for_iterable.kai",
        "fn main() {\n    let values: i32 = 5\n    for i in values {\n        print(i)\n    }\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode =
        kai::cli::runCompileCommand(sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_for.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("unsupported for-loop iterable") != std::string::npos);
    KAI_CHECK(err.str().find("LLVM IR generation failed") == std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// RELEASE HARDENING M2.1: an unsupported parameter type must ALSO get its
// own specific message (M2's second recordUnsupportedConstruct() call
// site, in declareFunction()'s parameter-type loop) - not the generic
// fallback. RETARGETED (KAI LANGUAGE M10B): a BARE slice parameter
// (`values: [i32]`) is now genuinely executable (spec §1/§20) - see
// SliceCodegenTests.cpp/the M10B native suite below for that positive
// coverage - so this test now uses an ARRAY that recursively contains a
// Slice (`[[i32]; 2]`), which remains explicitly unsupported (spec §6:
// `typeContainsSlice()` in LLVMCodeGenerator.cpp) and still produces the
// SAME diagnostic message this test has always locked in.
void testUnsupportedParameterTypeDiagnosticIsActionable() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_unsupported_param.kai",
        "fn sum(values: [[i32]; 2]) -> i32 {\n    return 0\n}\nfn main() {\n    print(0)\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_param.out", err);

    KAI_CHECK(exitCode == 6);
    KAI_CHECK(err.str().find("code generation is not yet supported for this parameter's type") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// --- KAI LANGUAGE M8B: array value copying + function ABI, native E2E ---
//
// M8A was a semantic-contract milestone only, and these three programs
// previously asserted a CLEAN BACKEND FAILURE (exit 6, an actionable
// unsupportedConstruct() message) through the full CLI, since no array
// function ABI or whole-array-copy codegen existed yet. KAI LANGUAGE M8B
// removes those guards and implements the approved by-value semantics as
// a direct LLVM aggregate ABI - these three are retargeted to run for
// real and assert exact stdout. See the A-M suite further below for the
// full M8B spec §20 matrix (independence, assignment, self-assignment,
// parameters, arguments, returns, no-aliasing, str/zero-length transport,
// evaluation order).

void testArrayParameterEndToEnd() {
    const CompileAndRunResult result =
        compileAndRun("kai_e2e_array_param.kai",
                       "fn sum(xs: [i32; 3]) -> i32 {\n    return xs[0] + xs[1] + xs[2]\n}\n"
                       "fn main() {\n    let a = [1, 2, 3]\n    print(sum(a))\n}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "6\n");
}

void testArrayReturnTypeEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_return.kai",
                                                       "fn make() -> [i32; 3] {\n    return [7, 8, 9]\n}\n"
                                                       "fn main() {\n"
                                                       "    let m = make()\n"
                                                       "    print(m[0])\n"
                                                       "    print(m[1])\n"
                                                       "    print(m[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "7\n8\n9\n");
}

// Whole-array initialization (`let b = a`, M8A §1/§18.A) is a real,
// independent value copy - see testArrayLocalCopyIndependenceEndToEnd()
// below for the mutation-independence proof.
void testWholeArrayInitializationEndToEnd() {
    const CompileAndRunResult result = compileAndRun(
        "kai_e2e_array_whole_copy.kai", "fn main() {\n    let a = [1, 2, 3]\n    let b = a\n    print(b[0])\n}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n");
}

// A. Local copy independence (spec §20.A): mutating the copy must not
// mutate the original.
void testArrayLocalCopyIndependenceEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_copy_independence.kai",
                                                       "fn main() {\n"
                                                       "    mut a = [1, 2, 3]\n"
                                                       "    mut b = a\n"
                                                       "    b[0] = 99\n"
                                                       "    print(a[0])\n"
                                                       "    print(b[0])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n99\n");
}

// B. Whole-array assignment (spec §20.B): `a = b` replaces every element.
void testArrayWholeAssignmentEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_whole_assignment.kai",
                                                       "fn main() {\n"
                                                       "    mut a = [1, 2, 3]\n"
                                                       "    let b = [4, 5, 6]\n"
                                                       "    a = b\n"
                                                       "    print(a[0])\n"
                                                       "    print(a[1])\n"
                                                       "    print(a[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "4\n5\n6\n");
}

// C. Self-assignment (spec §20.C): `a = a` must leave `a` unchanged.
void testArraySelfAssignmentEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_self_assignment.kai",
                                                       "fn main() {\n"
                                                       "    mut a = [1, 2, 3]\n"
                                                       "    a = a\n"
                                                       "    print(a[0])\n"
                                                       "    print(a[1])\n"
                                                       "    print(a[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n2\n3\n");
}

// E. Existing array value used as a call argument (spec §20.E).
void testArrayExistingValueAsArgumentEndToEnd() {
    const CompileAndRunResult result =
        compileAndRun("kai_e2e_array_arg_existing.kai",
                       "fn sum(xs: [i32; 3]) -> i32 {\n    return xs[0] + xs[1] + xs[2]\n}\n"
                       "fn main() {\n    let a = [1, 2, 3]\n    print(sum(a))\n}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "6\n");
}

// F. Inline array literal used directly as a call argument (spec §20.F).
void testArrayInlineLiteralAsArgumentEndToEnd() {
    const CompileAndRunResult result = compileAndRun(
        "kai_e2e_array_arg_literal.kai",
        "fn sum(xs: [i32; 3]) -> i32 {\n    return xs[0] + xs[1] + xs[2]\n}\nfn main() {\n    print(sum([10, 20, 30]))\n}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "60\n");
}

// H. Return of an EXISTING local array value, as opposed to G's inline
// literal (spec §20.G/§20.H).
void testArrayReturnExistingLocalEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_return_existing.kai",
                                                       "fn make() -> [i32; 3] {\n"
                                                       "    let a = [7, 8, 9]\n"
                                                       "    return a\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let m = make()\n"
                                                       "    print(m[0])\n"
                                                       "    print(m[1])\n"
                                                       "    print(m[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "7\n8\n9\n");
}

// I. Parameter -> return round trip (spec §20.I): the returned value has
// the SAME contents as what was passed in.
void testArrayParameterToReturnRoundTripEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_roundtrip.kai",
                                                       "fn echo(xs: [i32; 3]) -> [i32; 3] {\n    return xs\n}\n"
                                                       "fn main() {\n"
                                                       "    let a = [1, 2, 3]\n"
                                                       "    let b = echo(a)\n"
                                                       "    print(b[0])\n"
                                                       "    print(b[1])\n"
                                                       "    print(b[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n2\n3\n");
}

// J. No-alias round trip (spec §20.J): mutating the caller's `a` AFTER
// passing it by value into `echo()` must NOT affect the already-returned
// `b` - proves the callee received an independent copy, not a reference.
void testArrayNoAliasRoundTripEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_no_alias.kai",
                                                       "fn echo(xs: [i32; 3]) -> [i32; 3] {\n    return xs\n}\n"
                                                       "fn main() {\n"
                                                       "    mut a = [1, 2, 3]\n"
                                                       "    let b = echo(a)\n"
                                                       "    a[0] = 99\n"
                                                       "    print(a[0])\n"
                                                       "    print(b[0])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "99\n1\n");
}

// K. `str`-element array value transport through a parameter and return
// (spec §20.K) - `str` elements copy the view, never deep-copying text
// (TYPE_SYSTEM.md's existing `str` ownership design is unchanged by M8B).
void testArrayStrElementValueTransportEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_str.kai",
                                                       "fn first(xs: [str; 2]) -> str {\n    return xs[0]\n}\n"
                                                       "fn main() {\n"
                                                       "    let a = [\"hi\", \"bye\"]\n"
                                                       "    print(first(a))\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "hi\n");
}

// L. Zero-length array through a parameter AND a return (spec §20.L) -
// exercises the direct aggregate ABI at its degenerate `[N x T]` = `[0 x
// T]` case end to end.
void testArrayZeroLengthParameterAndReturnEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_zero_length.kai",
                                                       "fn echo(xs: [i32; 0]) -> [i32; 0] {\n    return xs\n}\n"
                                                       "fn main() {\n"
                                                       "    let a: [i32; 0] = []\n"
                                                       "    let b = echo(a)\n"
                                                       "    print(0)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n");
}

// M. An array-literal return value's elements are evaluated EXACTLY ONCE,
// left to right (spec §20.M) - each `sideEffect(n)` call prints once, in
// source order, strictly before the returned array's own elements are
// read back out by main().
void testArrayReturnLiteralEvaluationOrderEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_array_return_eval_order.kai",
                                                       "fn sideEffect(n: i32) -> i32 {\n"
                                                       "    print(n)\n"
                                                       "    return n\n"
                                                       "}\n"
                                                       "fn make() -> [i32; 3] {\n"
                                                       "    return [sideEffect(1), sideEffect(2), sideEffect(3)]\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let m = make()\n"
                                                       "    print(m[0])\n"
                                                       "    print(m[1])\n"
                                                       "    print(m[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n2\n3\n1\n2\n3\n");
}

// --- KAI LANGUAGE M9: nested fixed-array indexing, native E2E ---
//
// Before M9, `matrix[0][1]` failed codegen entirely (lowerArrayElementAddress()
// only accepted a direct identifier as an IndexExpr's object) - see
// LLVMCodeGeneratorTests.cpp's own M9 section header comment for the full
// root-cause writeup. M9's lowerArrayBase() generalizes this by recursing
// through nested IndexExpr layers; the frontend's own checkIndexExpr()
// already supported the nested TYPE rule (needed no change), and
// checkIndexAssignmentTarget() gained a root-identifier walk
// (unwrapIndexAssignmentRootIdentifier()) so mutability is still decided
// by the ROOT binding alone, however deeply nested the indexing above it.

// A. Basic 2D read (spec §21.A).
void testNestedArrayBasicReadEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_read.kai",
                                                       "fn main() {\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(m[0][0])\n"
                                                       "    print(m[0][1])\n"
                                                       "    print(m[1][0])\n"
                                                       "    print(m[1][1])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "1\n2\n3\n4\n");
}

// B. Nested write through a mutable local root (spec §21.B).
void testNestedArrayWriteEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_write.kai",
                                                       "fn main() {\n"
                                                       "    mut m = [[1, 2], [3, 4]]\n"
                                                       "    m[1][0] = 99\n"
                                                       "    print(m[1][0])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "99\n");
}

// C. Immutable nested write rejection (spec §8/§21.C): rejected in the
// FRONTEND (exit 5) via the SAME EXISTING AssignmentToImmutableBinding
// diagnostic a single-level `let xs = ...; xs[0] = 5` already gets - no
// new nested-specific diagnostic, and never a backend/LLVM crash.
void testNestedArrayImmutableWriteFailsInFrontend() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_nested_immutable_write.kai",
                         "fn main() {\n    let m = [[1, 2], [3, 4]]\n    m[1][0] = 99\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_nested_immutable.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("assignment to immutable binding") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// M9 FINAL CLEANUP: indexed mutation rooted at an ARRAY PARAMETER is
// rejected via the SAME EXISTING AssignmentToImmutableBinding diagnostic
// - never silently deferred to Unresolved, and never a new parameter-
// specific diagnostic - at single-level, 2-level nested, and 3-level
// nested depth (cleanup spec §1/§4.A-C).
void testArrayParameterIndexedWriteFailsInFrontendAtEveryNestingDepth() {
    struct Case {
        const char* fileName;
        const char* source;
    };
    const Case cases[] = {
        {"kai_e2e_param_write_single.kai", "fn f(xs: [i32; 3]) {\n    xs[0] = 99\n}\nfn main() {}"},
        {"kai_e2e_param_write_nested.kai",
         "fn f(m: [[i32; 2]; 2]) {\n    m[0][0] = 99\n}\nfn main() {}"},
        {"kai_e2e_param_write_3level.kai",
         "fn f(cube: [[[i32; 2]; 2]; 2]) {\n    cube[0][0][0] = 99\n}\nfn main() {}"},
    };

    for (const Case& testCase : cases) {
        const std::filesystem::path sourcePath = writeTempSource(testCase.fileName, testCase.source);
        std::error_code ignored;

        SourceManager sm;
        std::ostringstream err;
        const int exitCode = kai::cli::runCompileCommand(
            sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_param_write.out", err);

        KAI_CHECK(exitCode == 5);
        KAI_CHECK(err.str().find("assignment to immutable binding") != std::string::npos);

        std::filesystem::remove(sourcePath, ignored);
    }
}

// D. Row value copy / no alias (spec §10/§21.D): `mut row = matrix[1]`
// produces an INDEPENDENT array value - mutating `row` must never touch
// `matrix`'s own storage.
void testNestedArrayRowCopyNoAliasEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_row_copy.kai",
                                                       "fn main() {\n"
                                                       "    let matrix = [[1, 2], [3, 4]]\n"
                                                       "    mut row = matrix[1]\n"
                                                       "    row[0] = 99\n"
                                                       "    print(row[0])\n"
                                                       "    print(matrix[1][0])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "99\n3\n");
}

// E. Dynamic outer/inner indexes, both in bounds (spec §21.E).
void testNestedArrayDynamicIndexesInBoundsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_dyn_ok.kai",
                                                       "fn main() {\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    mut i: i32 = 1\n"
                                                       "    mut j: i32 = 1\n"
                                                       "    print(m[i][j])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "4\n");
}

// F. Outer dynamic out-of-bounds traps (spec §21.F). Same "no stable OS
// exit code, no output past the trapping access" contract M7B established
// for single-level dynamic OOB (spec §13).
void testNestedArrayOuterDynamicOutOfBoundsTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_outer_oob.kai",
                                                       "fn main() {\n"
                                                       "    mut i: i32 = 5\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(m[i][0])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// G. Inner dynamic out-of-bounds traps (spec §21.G).
void testNestedArrayInnerDynamicOutOfBoundsTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_inner_oob.kai",
                                                       "fn main() {\n"
                                                       "    mut j: i32 = 5\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(m[0][j])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// H. Negative signed outer AND inner index both trap (spec §21.H).
void testNestedArrayNegativeSignedOuterTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_neg_outer.kai",
                                                       "fn main() {\n"
                                                       "    mut i: i32 = 0 - 1\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(m[i][0])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

void testNestedArrayNegativeSignedInnerTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_neg_inner.kai",
                                                       "fn main() {\n"
                                                       "    mut j: i32 = 0 - 1\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(m[0][j])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// I. Unsigned nested indexing: in bounds succeeds, out of bounds traps
// (spec §21.I).
void testNestedArrayUnsignedInBoundsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_unsigned_ok.kai",
                                                       "fn main() {\n"
                                                       "    let m: [[i32; 2]; 2] = [[1, 2], [3, 4]]\n"
                                                       "    mut i: u32 = 1\n"
                                                       "    mut j: u32 = 0\n"
                                                       "    print(m[i][j])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n");
}

void testNestedArrayUnsignedOutOfBoundsTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_unsigned_oob.kai",
                                                       "fn main() {\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    mut j: u32 = 9\n"
                                                       "    print(m[0][j])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// J. Exactly-once / evaluation order, for both a nested READ (spec §5)
// and a nested WRITE's full `object[outer][inner] = value` chain (spec
// §15) - each side-effecting index/value expression appears in stdout
// EXACTLY once, strictly in source (left-to-right, outer-before-inner)
// order.
void testNestedArrayReadEvaluationOrderEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_eval_order_read.kai",
                                                       "fn outer() -> i32 {\n    print(100)\n    return 1\n}\n"
                                                       "fn inner() -> i32 {\n    print(200)\n    return 0\n}\n"
                                                       "fn main() {\n"
                                                       "    let matrix = [[1, 2], [3, 4]]\n"
                                                       "    print(matrix[outer()][inner()])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "100\n200\n3\n");
}

void testNestedArrayWriteEvaluationOrderEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_eval_order_write.kai",
                                                       "fn f() -> i32 {\n    print(100)\n    return 1\n}\n"
                                                       "fn g() -> i32 {\n    print(200)\n    return 0\n}\n"
                                                       "fn h() -> i32 {\n    print(300)\n    return 42\n}\n"
                                                       "fn main() {\n"
                                                       "    mut matrix = [[1, 2], [3, 4]]\n"
                                                       "    matrix[f()][g()] = h()\n"
                                                       "    print(matrix[1][0])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "100\n200\n300\n42\n");
}

// K. 3-level indexing (spec §16/§21.K) - the recursion is not hardcoded
// to depth 2.
void testNestedArrayThreeLevelIndexingEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_3level.kai",
                                                       "fn main() {\n"
                                                       "    let cube = [[[1, 2], [3, 4]], [[5, 6], [7, 8]]]\n"
                                                       "    print(cube[1][0][1])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "6\n");
}

// L. Nested-array parameter indexing (spec §11/§21.L) - no ABI change
// from M8B's direct aggregate strategy.
void testNestedArrayParameterIndexingEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_param.kai",
                                                       "fn get(m: [[i32; 2]; 2]) -> i32 {\n"
                                                       "    return m[1][0]\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(get(m))\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n");
}

// M. Returned nested-array indexing (spec §11/§21.M).
void testNestedArrayReturnIndexingEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_return.kai",
                                                       "fn make() -> [[i32; 2]; 2] {\n"
                                                       "    return [[1, 2], [3, 4]]\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let m = make()\n"
                                                       "    print(m[1][1])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "4\n");
}

// N. Zero-length nested-dimension OOB behavior (spec §18/§21.N): a
// CONSTANT index into a zero-length dimension is a compile-time error
// (the SAME ArrayIndexOutOfBounds diagnostic every other constant OOB
// index gets - never a new "multidimensional bounds" diagnostic), and a
// DYNAMIC index into one traps at runtime, same as any other dynamic OOB.
void testNestedArrayZeroLengthDimensionConstantOOBFailsInFrontend() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_nested_zero_length_const_oob.kai",
        "fn main() {\n    let m: [[i32; 0]; 2] = [[], []]\n    print(m[0][0])\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_nested_zero_length_const_oob.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("array index out of bounds") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

void testNestedArrayZeroLengthDimensionDynamicTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_nested_zero_length_dyn_oob.kai",
                                                       "fn main() {\n"
                                                       "    let m: [[i32; 0]; 2] = [[], []]\n"
                                                       "    mut j = 0\n"
                                                       "    print(m[0][j])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
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
    // NativeLinker::link() itself has no output-naming policy - it links
    // to exactly the path it is given (that policy lives one layer up, in
    // kai::cli::resolveNativeExecutablePath() - see CompileCommand.cpp).
    // This test applies the same resolution a real caller would, so it
    // asks the host driver to produce a path it will actually create on
    // every platform (WINDOWS M1 spec §7).
    const std::filesystem::path outputPath =
        kai::cli::resolveNativeExecutablePath(std::filesystem::temp_directory_path() / "kai_link_test.out");
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
    KAI_CHECK_STDOUT_BYTES(stdoutText, "7\n");

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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "Hello, KAI!\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "Welcome to KAI\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, expected);
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, std::string("a\0b\n", 4));
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "KAI ✓\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "Hello, KAI!\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "KAI\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "hello\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "typed\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "forwarded\n");
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, std::string("a\0b\n", 4));
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
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "KAI ✓\n");
}

// --- KAI LANGUAGE M10B: immutable slice VALUES + checked indexing,
// native E2E ---
//
// M10A gave `[T]` a real semantic Type but no runtime representation or
// codegen at all. M10B implements the approved scope: `slice(array)` as
// an explicit, non-owning view construction; local Slice bindings/copy;
// checked Slice indexed reads; `len(...)` over a fixed array/Slice/str;
// and Slice function parameters (by value, copy-the-view). Slice
// RETURNS and any executable aggregate recursively containing a Slice
// remain deliberately rejected - see tests O/P below.

// A. Basic local Slice: construction, len(), checked reads.
void testSliceBasicLocalEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_basic.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    print(len(s))\n"
                                                       "    print(s[0])\n"
                                                       "    print(s[2])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n10\n30\n");
}

// B. Slice copy: `let t = s` copies the VIEW only - both `s` and `t`
// observe the exact same length/elements (spec §12/§22).
void testSliceCopyEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_copy.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    let t = s\n"
                                                       "    print(len(s))\n"
                                                       "    print(len(t))\n"
                                                       "    print(s[1])\n"
                                                       "    print(t[1])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n3\n20\n20\n");
}

// C. Slice view REBINDING: `mut s` may be reassigned to an entirely
// different view under KAI's ordinary binding-mutability rules (spec
// §13) - this rebinds the VIEW, never mutates any array's elements.
void testSliceRebindingEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_rebind.kai",
                                                       "fn main() {\n"
                                                       "    let a = [1, 2, 3]\n"
                                                       "    let b = [4, 5, 6, 7]\n"
                                                       "    mut s = slice(a)\n"
                                                       "    print(len(s))\n"
                                                       "    s = slice(b)\n"
                                                       "    print(len(s))\n"
                                                       "    print(s[3])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n4\n7\n");
}

// D. Slice function PARAMETER: `sum(slice(values))`, iterating via
// `for i in 0..len(xs)` (spec §20 - the exact example the spec gives).
void testSliceParameterSumEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_param_sum.kai",
                                                       "fn sum(xs: [i32]) -> i32 {\n"
                                                       "    mut result: i32 = 0\n"
                                                       "    for i in 0..len(xs) {\n"
                                                       "        result = result + xs[i]\n"
                                                       "    }\n"
                                                       "    return result\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let values = [10, 20, 30]\n"
                                                       "    print(sum(slice(values)))\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "60\n");
}

// E. Explicit conversion is REQUIRED: passing a fixed array directly
// where a Slice parameter is expected remains a genuine, FRONTEND
// TypeMismatch (spec §2) - never an implicit array-to-slice conversion,
// and never an LLVM/backend error.
void testImplicitArrayToSliceConversionFailsInFrontend() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_slice_implicit_conversion.kai",
                         "fn sum(xs: [i32]) -> i32 {\n    return 0\n}\n"
                         "fn main() {\n    let a = [1, 2, 3]\n    print(sum(a))\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_slice_implicit.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("type mismatch") != std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// F. Dynamic signed index, in bounds.
void testSliceDynamicSignedInBoundsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_dyn_signed_ok.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i: i32 = 1\n"
                                                       "    print(s[i])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "20\n");
}

// G. Dynamic signed NEGATIVE index traps - a directly-known negative
// constant is caught at compile time (see the semantic test suite
// instead); this is the genuinely dynamic case.
void testSliceDynamicSignedNegativeTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_dyn_signed_negative.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i: i32 = 0 - 1\n"
                                                       "    print(s[i])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// H. Dynamic upper-bound OOB traps - against the Slice's own RUNTIME
// length, not any originating array's compile-time length.
void testSliceDynamicUpperBoundTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_dyn_upper_oob.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i: i32 = 3\n"
                                                       "    print(s[i])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// I. Dynamic UNSIGNED index: in bounds succeeds, out of bounds traps.
void testSliceDynamicUnsignedInBoundsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_dyn_unsigned_ok.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i: u32 = 2\n"
                                                       "    print(s[i])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "30\n");
}

void testSliceDynamicUnsignedOutOfBoundsTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_dyn_unsigned_oob.kai",
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i: u32 = 3\n"
                                                       "    print(s[i])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// J. Index evaluated EXACTLY ONCE - a side-effecting index expression
// must print exactly once, and the final read reflects that ONE call's
// result.
void testSliceIndexEvaluationOrderEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_eval_order.kai",
                                                       "fn idx() -> i32 {\n"
                                                       "    print(100)\n"
                                                       "    return 1\n"
                                                       "}\n"
                                                       "fn main() {\n"
                                                       "    let a = [10, 20, 30]\n"
                                                       "    let s = slice(a)\n"
                                                       "    print(s[idx()])\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "100\n20\n");
}

// K. Zero-length Slice: len() is 0, and any element access (constant or
// dynamic) traps - there is no valid index into an empty view.
void testSliceZeroLengthLenEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_zero_len.kai",
                                                       "fn main() {\n"
                                                       "    let a: [i32; 0] = []\n"
                                                       "    let s = slice(a)\n"
                                                       "    print(len(s))\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "0\n");
}

void testSliceZeroLengthDynamicAccessTrapsEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_slice_zero_len_access.kai",
                                                       "fn main() {\n"
                                                       "    let a: [i32; 0] = []\n"
                                                       "    let s = slice(a)\n"
                                                       "    mut i = 0\n"
                                                       "    print(s[i])\n"
                                                       "    print(888)\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    if (!result.compileSucceeded) {
        return;
    }
    KAI_CHECK(result.runExitCode != 0);
    KAI_CHECK(result.stdoutText.find("888") == std::string::npos);
}

// L. len(fixed array), including a NESTED array - the outermost
// dimension's length only, no multidimensional special case (spec §23).
void testLenOfFixedArrayEndToEnd() {
    const CompileAndRunResult result = compileAndRun("kai_e2e_len_array.kai",
                                                       "fn main() {\n"
                                                       "    let a = [1, 2, 3]\n"
                                                       "    let m = [[1, 2], [3, 4]]\n"
                                                       "    print(len(a))\n"
                                                       "    print(len(m))\n"
                                                       "}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n2\n");
}

// M. len(str) - the existing str byte-length contract, bytes only, no
// UTF-8 codepoint/grapheme counting (spec §24) - "KAI" is 3 ASCII bytes,
// "✓" is a 3-byte UTF-8 sequence, so "KAI✓" is 6 bytes total.
void testLenOfStrEndToEnd() {
    const CompileAndRunResult result =
        compileAndRun("kai_e2e_len_str.kai", "fn main() {\n    print(len(\"abc\"))\n    print(len(\"KAI✓\"))\n}");

    KAI_CHECK(result.compileSucceeded);
    KAI_CHECK(result.runExitCode == 0);
    KAI_CHECK_STDOUT_BYTES(result.stdoutText, "3\n6\n");
}

// N. Slice indexed mutation is rejected in the FRONTEND (exit 5) via the
// dedicated AssignmentThroughImmutableSlice diagnostic - never
// AssignmentToImmutableBinding (the slice binding itself is `mut` here),
// and never a backend/LLVM crash.
void testSliceIndexedMutationFailsInFrontend() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_slice_mutation.kai",
                         "fn main() {\n    let a = [1, 2, 3]\n    mut s = slice(a)\n    s[0] = 5\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_slice_mutation.out", err);

    KAI_CHECK(exitCode == 5);
    KAI_CHECK(err.str().find("assignment through immutable slice") != std::string::npos);
    KAI_CHECK(err.str().find("assignment to immutable binding") == std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// O. A Slice RETURN is rejected cleanly at the BACKEND (exit 6) - it is
// semantically well-typed (TypeChecker accepts it fine), so this fails
// at code generation specifically, never a frontend semantic error and
// never a crash (spec §5/§23).
void testSliceReturnFailsCleanlyAtBackend() {
    const std::filesystem::path sourcePath = writeTempSource(
        "kai_e2e_slice_return.kai", "fn bad(xs: [i32]) -> [i32] {\n    return xs\n}\nfn main() {\n    print(0)\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_slice_return.out", err);

    KAI_CHECK(exitCode == 6);
    KAI_CHECK(err.str().find("code generation is not yet supported for this function's return type") !=
              std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

// P. An executable aggregate that recursively contains a Slice (an array
// RETURN type of `[[i32]; 2]`) is rejected cleanly at the BACKEND too -
// preventing indirect escape of a borrowed view through M8's own array
// return machinery (spec §6/§28).
void testArrayContainingSliceEscapeFailsCleanlyAtBackend() {
    const std::filesystem::path sourcePath =
        writeTempSource("kai_e2e_slice_escape.kai",
                         "fn make(xs: [i32]) -> [[i32]; 2] {\n    return [xs, xs]\n}\nfn main() {\n    print(0)\n}");
    std::error_code ignored;

    SourceManager sm;
    std::ostringstream err;
    const int exitCode = kai::cli::runCompileCommand(
        sm, sourcePath, std::filesystem::temp_directory_path() / "kai_e2e_slice_escape.out", err);

    KAI_CHECK(exitCode == 6);
    KAI_CHECK(err.str().find("code generation is not yet supported for this function's return type") !=
              std::string::npos);

    std::filesystem::remove(sourcePath, ignored);
}

} // namespace

int main() {
    testHelloPrint42EndToEnd();
    testFactorialEndToEnd();
    testWhileLoopEndToEnd();

    testForLoopLiteralRangeEndToEnd();
    testForLoopZeroIterationsWhenStartEqualsEndEndToEnd();
    testForLoopZeroIterationsWhenStartExceedsEndEndToEnd();
    testForLoopRangeFromVariablesEndToEnd();
    testForLoopNestedEndToEnd();
    testForLoopReturnInsideBodyEndToEnd();
    testForLoopOuterVariableShadowingEndToEnd();
    testForLoopEndpointsEvaluatedOnceEndToEnd();

    testArrayLiteralIndexedReadEndToEnd();
    testArrayIndexedWriteEndToEnd();
    testArrayM6ForRangeReadEndToEnd();
    testArrayM6ForRangeMutationEndToEnd();
    testArrayLiteralEvaluationOrderEndToEnd();
    testArrayDynamicSignedInBoundsEndToEnd();
    testArrayDynamicSignedUpperBoundTrapsEndToEnd();
    testArrayDynamicSignedNegativeTrapsEndToEnd();
    testArrayDynamicUnsignedInBoundsEndToEnd();
    testArrayDynamicUnsignedOutOfBoundsTrapsEndToEnd();
    testArrayImmutableIndexedAssignmentFailsInFrontend();
    testArrayCompileTimeOutOfBoundsDiagnostics();

    testCompileFailsCleanlyOnSemanticError();
    testCompileFailsCleanlyWithNoMain();

    testTypeMismatchDiagnosticIncludesExpectedAndActualTypes();
    testLiteralOutOfRangeDiagnosticIncludesTargetType();
    testUnsupportedForIterableDiagnosticIsActionable();
    testUnsupportedParameterTypeDiagnosticIsActionable();
    testArrayParameterEndToEnd();
    testArrayReturnTypeEndToEnd();
    testWholeArrayInitializationEndToEnd();
    testArrayLocalCopyIndependenceEndToEnd();
    testArrayWholeAssignmentEndToEnd();
    testArraySelfAssignmentEndToEnd();
    testArrayExistingValueAsArgumentEndToEnd();
    testArrayInlineLiteralAsArgumentEndToEnd();
    testArrayReturnExistingLocalEndToEnd();
    testArrayParameterToReturnRoundTripEndToEnd();
    testArrayNoAliasRoundTripEndToEnd();
    testArrayStrElementValueTransportEndToEnd();
    testArrayZeroLengthParameterAndReturnEndToEnd();
    testArrayReturnLiteralEvaluationOrderEndToEnd();

    testNestedArrayBasicReadEndToEnd();
    testNestedArrayWriteEndToEnd();
    testNestedArrayImmutableWriteFailsInFrontend();
    testArrayParameterIndexedWriteFailsInFrontendAtEveryNestingDepth();
    testNestedArrayRowCopyNoAliasEndToEnd();
    testNestedArrayDynamicIndexesInBoundsEndToEnd();
    testNestedArrayOuterDynamicOutOfBoundsTrapsEndToEnd();
    testNestedArrayInnerDynamicOutOfBoundsTrapsEndToEnd();
    testNestedArrayNegativeSignedOuterTrapsEndToEnd();
    testNestedArrayNegativeSignedInnerTrapsEndToEnd();
    testNestedArrayUnsignedInBoundsEndToEnd();
    testNestedArrayUnsignedOutOfBoundsTrapsEndToEnd();
    testNestedArrayReadEvaluationOrderEndToEnd();
    testNestedArrayWriteEvaluationOrderEndToEnd();
    testNestedArrayThreeLevelIndexingEndToEnd();
    testNestedArrayParameterIndexingEndToEnd();
    testNestedArrayReturnIndexingEndToEnd();
    testNestedArrayZeroLengthDimensionConstantOOBFailsInFrontend();
    testNestedArrayZeroLengthDimensionDynamicTrapsEndToEnd();

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

    testSliceBasicLocalEndToEnd();
    testSliceCopyEndToEnd();
    testSliceRebindingEndToEnd();
    testSliceParameterSumEndToEnd();
    testImplicitArrayToSliceConversionFailsInFrontend();
    testSliceDynamicSignedInBoundsEndToEnd();
    testSliceDynamicSignedNegativeTrapsEndToEnd();
    testSliceDynamicUpperBoundTrapsEndToEnd();
    testSliceDynamicUnsignedInBoundsEndToEnd();
    testSliceDynamicUnsignedOutOfBoundsTrapsEndToEnd();
    testSliceIndexEvaluationOrderEndToEnd();
    testSliceZeroLengthLenEndToEnd();
    testSliceZeroLengthDynamicAccessTrapsEndToEnd();
    testLenOfFixedArrayEndToEnd();
    testLenOfStrEndToEnd();
    testSliceIndexedMutationFailsInFrontend();
    testSliceReturnFailsCleanlyAtBackend();
    testArrayContainingSliceEscapeFailsCleanlyAtBackend();

    return kai::test::failureCount == 0 ? 0 : 1;
}
