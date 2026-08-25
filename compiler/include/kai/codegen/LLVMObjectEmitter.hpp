#pragma once

#include <filesystem>
#include <ostream>

namespace llvm {
class Module;
} // namespace llvm

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 7: lowers an already-verified llvm::Module (the
/// output of LLVMCodeGenerator::generate()) into a native, host-triple
/// object file - the second stage of the M7 pipeline:
///
///     AST + SemanticModel -> LLVM IR         (LLVMCodeGenerator)
///     LLVM Module -> native .o               (this class)
///     .o + KAI runtime -> native executable  (NativeLinker)
///
/// MVP scope: host-native (Fedora Linux x86_64) object emission only - no
/// cross-compilation, no optimization passes, no debug info (M7 spec
/// §5/§29/§30). This class performs no KAI-authored codegen logic of its
/// own: it drives LLVM's own target-machine object-emission pipeline,
/// exactly what `llc -filetype=obj` itself does.
class LLVMObjectEmitter {
public:
    /// One-time, process-wide native target/target-info/asm-printer
    /// registration (llvm::InitializeNativeTarget() +
    /// llvm::InitializeNativeTargetAsmPrinter() - see .cpp). Registers
    /// ONLY the host architecture's backend, never every LLVM-supported
    /// target (M7 spec §6). Safe to call more than once - the underlying
    /// LLVMInitializeNativeTarget() family is itself idempotent.
    static void initializeNativeTarget();

    /// Adapts `module`'s source-level KAI `main` (if any) to the
    /// platform's native process-entry ABI, in place, BEFORE emit():
    ///
    ///   fn main() { ... }        (LLVM `void @main()`, zero parameters)
    ///     -> renamed to an internal-linkage `__kai_user_main` (every
    ///        existing IR reference follows the SAME llvm::Function
    ///        object across the rename, so nothing else needs updating),
    ///        then wrapped by a newly created, external-linkage
    ///        `define i32 @main() { call void @__kai_user_main()
    ///        ret i32 0 }` - SYNTAX.md §7 documents implicit-Unit `main`
    ///        as ordinary KAI source, not itself already native-ABI-
    ///        shaped, so it needs this adaptation to become a valid
    ///        `int main(void)`.
    ///
    ///   fn main() -> i32 { ... } (LLVM `i32 @main()`, zero parameters)
    ///     -> used directly, unmodified - SYNTAX.md §7 already documents
    ///        this shape as supported KAI syntax, and it already matches
    ///        the native `int main(void)` ABI exactly, so no wrapper is
    ///        needed.
    ///
    /// Fails explicitly (returns false, writes one message to `err`,
    /// mutates nothing further) for: no `main` function in `module`;
    /// `main` with one or more parameters; or `main` with any LLVM return
    /// type other than void/i32 - this MVP policy never fabricates a
    /// native entry for a return shape it does not understand (M7 spec
    /// §10: no silently selecting another function, no fabricated return
    /// value).
    static bool adaptNativeEntryPoint(llvm::Module& module, std::ostream& err);

    /// Emits `module` (expected already verified by
    /// LLVMCodeGenerator::generate(), and already passed through
    /// adaptNativeEntryPoint()) as a native object file at `outputPath`:
    /// looks up the host target via TargetRegistry, creates a
    /// TargetMachine for it, sets `module`'s OWN target triple/DataLayout
    /// from that TargetMachine (M7 spec §5 - never a hard-coded triple),
    /// opens `outputPath`, and runs LLVM's own object-emission codegen
    /// pass pipeline against `module`. No optimization passes are added
    /// (M7 spec §29 - CodeGenOptLevel::None). Returns false and writes
    /// one message to `err` on any failure (target lookup, TargetMachine
    /// creation, output-file open, the emission pass itself, or a
    /// write-time I/O error) - never leaves a file that looks like a
    /// successful object after a failure.
    static bool emit(llvm::Module& module, const std::filesystem::path& outputPath, std::ostream& err);
};

} // namespace kai::codegen
