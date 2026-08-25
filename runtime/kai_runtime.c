#include "kai_runtime.h"

#include <stdio.h>

/* MVP newline policy (LLVM CODEGEN MILESTONE 6 spec #16): each `print`
 * call emits its value followed by a newline - no existing KAI doc
 * commits the prelude `print` to the opposite ("no newline") behavior,
 * and one-line-per-call output is what every existing examples directory
 * demo (fibonacci.kai, conditions.kai, ...) already reads as intending.
 */

void kai_print_i64(int64_t value) {
    printf("%lld\n", (long long)value);
}

void kai_print_u64(uint64_t value) {
    printf("%llu\n", (unsigned long long)value);
}

void kai_print_bool(int32_t value) {
    fputs(value != 0 ? "true\n" : "false\n", stdout);
}

void kai_print_f64(double value) {
    printf("%g\n", value);
}

void kai_print_str(const char* data, uint64_t length) {
    /* fwrite, never strlen/fputs: `length` is the DECODED byte length and
     * may legally exceed the position of an embedded '\0' escape byte
     * inside `data` - a NUL-terminated write would silently truncate
     * valid KAI string data (see kai_runtime.h's own comment). */
    fwrite(data, 1, (size_t)length, stdout);
    fputc('\n', stdout);
}
