#include "kai_runtime.h"

#include <stdio.h>

#ifdef _WIN32
/* WINDOWS M1.1: the ONLY platform-conditional #include in this file -
 * <fcntl.h>/<io.h> for _setmode()/_O_BINARY/_fileno() below. MinGW-w64's
 * UCRT64 CRT provides the exact same names/semantics as MSVC's here
 * (both target the same underlying Universal CRT), so no separate
 * MSVC-vs-MinGW branch is needed. */
#include <fcntl.h>
#include <io.h>
#endif

/* MVP newline policy (LLVM CODEGEN MILESTONE 6 spec #16): each `print`
 * call emits its value followed by a newline - no existing KAI doc
 * commits the prelude `print` to the opposite ("no newline") behavior,
 * and one-line-per-call output is what every existing examples directory
 * demo (fibonacci.kai, conditions.kai, ...) already reads as intending.
 *
 * WINDOWS M1.1 (deterministic byte output): that newline is always
 * exactly one LF byte (0x0A) on every supported platform - see the root
 * README.md's "Output byte semantics" note. Windows' C runtime (this is
 * standard, documented CRT behavior, true of both MSVC's UCRT and
 * MinGW-w64's UCRT64 build - not a MinGW quirk) opens stdin/stdout/stderr
 * in TEXT mode by default, which silently rewrites every LF a buffered
 * stdio call (printf/fputs/fwrite/fputc) writes into CRLF (0x0D 0x0A) on
 * its way out - regardless of whether the destination is a console, a
 * redirected file, or a pipe, since the translation happens inside the
 * CRT's own stream layer, not at the OS handle. kai_prepare_stdout()
 * below switches stdout to BINARY mode (_O_BINARY) once, before the
 * first byte of KAI output, so every kai_print_* function here emits
 * exactly the bytes it constructs - never a runtime-injected extra
 * 0x0D. This is a stdout-stream-mode change only: it does not touch what
 * bytes any kai_print_* function constructs (kai_print_str's
 * length-aware fwrite() below is completely unaffected either way), and
 * it is a no-op on every non-Windows platform, compiling away entirely
 * there. */
static void kai_prepare_stdout(void) {
#ifdef _WIN32
    /* KAI 0.1 has no threads (LANGUAGE_DESIGN.md/KAI_0_1_SCOPE.md: no
     * concurrency in this milestone), so a plain (non-atomic) static
     * guard is proportional to the runtime's current scope - there is no
     * concurrent caller for this to race against today. Revisit if/when
     * KAI gains concurrency. _setmode() itself is also idempotent, so a
     * duplicate call from a second, later-added print function would be
     * harmless even without this guard - the guard exists to avoid
     * calling it on every single print, not for correctness. */
    static int stdoutPrepared = 0;
    if (!stdoutPrepared) {
        _setmode(_fileno(stdout), _O_BINARY);
        stdoutPrepared = 1;
    }
#endif
}

void kai_print_i64(int64_t value) {
    kai_prepare_stdout();
    printf("%lld\n", (long long)value);
}

void kai_print_u64(uint64_t value) {
    kai_prepare_stdout();
    printf("%llu\n", (unsigned long long)value);
}

void kai_print_bool(int32_t value) {
    kai_prepare_stdout();
    fputs(value != 0 ? "true\n" : "false\n", stdout);
}

void kai_print_f64(double value) {
    kai_prepare_stdout();
    printf("%g\n", value);
}

void kai_print_str(const char* data, uint64_t length) {
    kai_prepare_stdout();
    /* fwrite, never strlen/fputs: `length` is the DECODED byte length and
     * may legally exceed the position of an embedded '\0' escape byte
     * inside `data` - a NUL-terminated write would silently truncate
     * valid KAI string data (see kai_runtime.h's own comment). */
    fwrite(data, 1, (size_t)length, stdout);
    fputc('\n', stdout);
}
