#pragma once

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 7: the third and final stage of the M7 pipeline
/// - `.o` + KAI runtime -> native executable:
///
///     AST + SemanticModel -> LLVM IR         (LLVMCodeGenerator)
///     LLVM Module -> native .o               (LLVMObjectEmitter)
///     .o + KAI runtime -> native executable  (this class)
///
/// Deliberately invokes the HOST C COMPILER DRIVER (`cc`/`clang`/`gcc`) as
/// the linker, rather than invoking `ld` directly or hand-reconstructing
/// glibc's startup/linker arguments (M7 spec §12): the driver already
/// knows how to supply the correct C runtime startup objects (crt1.o/
/// crti.o/...) and libc for the KAI runtime's own printf/fputs calls.
/// Every child-process invocation goes through llvm::sys::ExecuteAndWait
/// with a structural argument array - never std::system() or any other
/// shell-interpreted command line - so there is no shell-quoting/
/// injection concern for any of these filesystem paths (M7 spec §12).
class NativeLinker {
public:
    /// Finds a usable host C compiler driver to invoke as the linker.
    /// Lookup order:
    ///   1. the `KAI_CC` environment variable, if set (explicit override -
    ///      lets a development/test environment pin an exact driver
    ///      without touching PATH).
    ///   2. `cc`
    ///   3. `clang`
    ///   4. `gcc`
    /// Each candidate is resolved via llvm::sys::findProgramByName (PATH
    /// search, no shell). Returns the first candidate found, as its full
    /// resolved path; std::nullopt if none of them exist.
    static std::optional<std::string> findCompilerDriver();

    /// Locates the default static KAI runtime archive (`libkai_runtime.a`,
    /// built by M6 - see runtime/kai_runtime.h) to link into a generated
    /// executable. `kaiccExecutablePath` is the path to the CURRENTLY
    /// RUNNING kaicc executable (see currentExecutablePath()) - this is
    /// what makes the lookup relocatable: it never depends on where the
    /// build happened to place things on the machine that built kaicc,
    /// only on where kaicc ITSELF is installed right now (needed once
    /// kaicc + the runtime are bundled into e.g. a VS Code extension).
    /// Lookup order:
    ///   1. the `KAI_RUNTIME_LIB` environment variable, if set AND the
    ///      path it names exists (explicit override - genuinely useful
    ///      for tests/development, never required for ordinary use).
    ///   2. `<dir containing kaiccExecutablePath>/../lib/kai/libkai_runtime.a`
    ///      - the packaged/installed layout this project's own CMake
    ///      build already arranges (`build/bin/kaicc` +
    ///      `build/lib/kai/libkai_runtime.a` - see the top-level
    ///      CMakeLists.txt's RUNTIME_OUTPUT_DIRECTORY/
    ///      ARCHIVE_OUTPUT_DIRECTORY settings), so this same lookup
    ///      already works unmodified in the build tree - no separate
    ///      build-tree-only fallback was needed.
    ///   3. `<dir containing kaiccExecutablePath>/libkai_runtime.a` - a
    ///      flatter fallback layout, checked only if (2) does not exist.
    /// Returns std::nullopt if none of these exist on disk - never a
    /// path that has not actually been verified to exist.
    static std::optional<std::filesystem::path>
    findDefaultRuntimeLibrary(const std::filesystem::path& kaiccExecutablePath);

    /// The absolute path to the currently running executable, via Linux's
    /// `/proc/self/exe` (Fedora Linux x86_64 is this MVP's only supported
    /// host - M7 spec §5) - robust regardless of how the process was
    /// invoked (a bare name found via PATH, a relative path, a symlink),
    /// unlike parsing argv[0] directly. Returns an empty path if the
    /// symlink cannot be read.
    static std::filesystem::path currentExecutablePath();

    /// Links `objectPath` and `runtimeLibraryPath` into a native
    /// executable at `outputPath` by invoking `compilerDriverPath`
    /// (e.g. the result of findCompilerDriver()) as a child process:
    ///
    ///     <compilerDriverPath> <objectPath> <runtimeLibraryPath> -o <outputPath>
    ///
    /// The child inherits this process's stdout/stderr directly (no
    /// output capture/redirection) - a real link error from the
    /// compiler driver is therefore already visible to whoever is running
    /// kaicc, with no need to duplicate it. Returns false (after writing
    /// one additional kaicc-level message to `err`) if the driver could
    /// not be executed at all, or exited with a nonzero status.
    static bool link(const std::string& compilerDriverPath, const std::filesystem::path& objectPath,
                      const std::filesystem::path& runtimeLibraryPath, const std::filesystem::path& outputPath,
                      std::ostream& err);
};

} // namespace kai::codegen
