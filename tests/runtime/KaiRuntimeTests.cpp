#include "kai_runtime.h"

#include "support/check.hpp"

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

int main() {
    testPrintI64DoesNotCrash();
    testPrintU64DoesNotCrash();
    testPrintBoolDoesNotCrash();
    testPrintF64DoesNotCrash();

    return kai::test::failureCount == 0 ? 0 : 1;
}
