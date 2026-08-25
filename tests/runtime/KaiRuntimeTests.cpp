#include "kai_runtime.h"

#include "support/check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

// LLVM CODEGEN MILESTONE 6: proves the tiny native runtime links and its
// exported C ABI symbols can actually be called - not a formatting/output
// test (see this file's own CMakeLists.txt registration comment for why
// captured-stdout verification is deferred to M7). If any of these calls
// crashed or failed to link, this executable would not reach `return 0`
// at all.

void testPrintI64DoesNotCrash() {
    kai_print_i64(0);
    kai_print_i64(42);
    kai_print_i64(-42);
}

void testPrintU64DoesNotCrash() {
    kai_print_u64(0);
    kai_print_u64(42);
}

void testPrintBoolDoesNotCrash() {
    kai_print_bool(0);
    kai_print_bool(1);
}

void testPrintF64DoesNotCrash() {
    kai_print_f64(0.0);
    kai_print_f64(3.5);
    kai_print_f64(-3.5);
}

// Minimal String Literal Support milestone: unlike the "does not crash"
// smoke tests above, kai_print_str's exact byte-for-byte fwrite() behavior
// is proven directly here (not left solely to codegen/native-execution
// tests - M8 spec §22) by redirecting the real process stdout fd to a
// temp file for the duration of one call, then reading it back and
// comparing bytes exactly (std::string, never a C-string/strlen-based
// comparison, since an embedded '\0' must survive intact).
std::string captureKaiPrintStrOutput(const char* data, std::uint64_t length) {
    const std::filesystem::path capturePath =
        std::filesystem::temp_directory_path() / "kai_runtime_test_print_str_capture.bin";
    std::error_code ignored;
    std::filesystem::remove(capturePath, ignored);

    std::fflush(stdout);
    const int savedStdoutFd = dup(fileno(stdout));
    std::freopen(capturePath.string().c_str(), "wb", stdout);

    kai_print_str(data, length);
    std::fflush(stdout);

    dup2(savedStdoutFd, fileno(stdout));
    close(savedStdoutFd);
    clearerr(stdout);

    std::ifstream capture(capturePath, std::ios::binary);
    std::ostringstream buffer;
    buffer << capture.rdbuf();
    std::filesystem::remove(capturePath, ignored);
    return buffer.str();
}

void testPrintStrExactBytesWithExplicitLength() {
    const std::string output = captureKaiPrintStrOutput("hello", 5);
    KAI_CHECK(output == std::string("hello\n"));
}

// REQUIRED (M8 spec #6/#14/#22): the length argument, not strlen(), must
// govern how many bytes are written - an embedded '\0' must print intact,
// never truncate the output.
void testPrintStrEmbeddedNulExactBytes() {
    const char data[] = {'a', '\0', 'b'};
    const std::string output = captureKaiPrintStrOutput(data, 3);
    KAI_CHECK(output.size() == 4);
    KAI_CHECK(output == std::string("a\0b\n", 4));
}

void testPrintStrZeroLength() {
    const std::string output = captureKaiPrintStrOutput("", 0);
    KAI_CHECK(output == std::string("\n"));
}

int main() {
    testPrintI64DoesNotCrash();
    testPrintU64DoesNotCrash();
    testPrintBoolDoesNotCrash();
    testPrintF64DoesNotCrash();

    testPrintStrExactBytesWithExplicitLength();
    testPrintStrEmbeddedNulExactBytes();
    testPrintStrZeroLength();

    return kai::test::failureCount == 0 ? 0 : 1;
}
