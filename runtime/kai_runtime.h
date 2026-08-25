#ifndef KAI_RUNTIME_H
#define KAI_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tiny native runtime KAI-generated LLVM IR calls into (LLVM CODEGEN
 * MILESTONE 6). Each function below is a stable, unmangled C ABI symbol:
 * LLVMCodeGenerator's lowerPrintCall() only ever `declare`s/`call`s these
 * by name into a generated module - it never links or calls into this
 * library directly (compiler and runtime are deliberately separate
 * responsibilities/CMake targets - see the top-level CMakeLists.txt).
 * M7 is expected to link this runtime's own object code into the final
 * native executable alongside a generated module's object file.
 *
 * Naming: `kai_print_*`, not plain `print`/`printf`, to avoid colliding
 * with user KAI function names or system symbols at link time.
 *
 * Width/sign policy: rather than one runtime helper per KAI integer
 * width, codegen normalizes every signed integer to i64 (sign-extended)
 * and every unsigned integer to a 64-bit value (zero-extended) before
 * calling - see lowerPrintCall()'s own doc comment. `kai_print_bool`
 * takes an ordinary 32-bit int (0/false, nonzero/true) rather than a C
 * `_Bool`/LLVM i1 directly, to stay clear of any C-ABI bit-width
 * ambiguity at the call boundary.
 */

void kai_print_i64(int64_t value);
void kai_print_u64(uint64_t value);
void kai_print_bool(int32_t value);
void kai_print_f64(double value);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KAI_RUNTIME_H
