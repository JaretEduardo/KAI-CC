#include "kai/codegen/NativeLinker.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Program.h>

#include <array>
#include <cstdlib>
#include <system_error>

namespace kai::codegen {

namespace {

// Tries a single candidate driver name/path through
// llvm::sys::findProgramByName (a PATH search, never a shell) - returns
// its resolved path on success, std::nullopt otherwise.
std::optional<std::string> tryFindProgram(llvm::StringRef name) {
    const llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName(name);
    if (!found) {
        return std::nullopt;
    }
    return *found;
}

} // namespace

std::optional<std::string> NativeLinker::findCompilerDriver() {
    if (const char* envOverride = std::getenv("KAI_CC")) {
        if (std::optional<std::string> resolved = tryFindProgram(envOverride)) {
            return resolved;
        }
    }

    for (const char* candidate : {"cc", "clang", "gcc"}) {
        if (std::optional<std::string> resolved = tryFindProgram(candidate)) {
            return resolved;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path>
NativeLinker::findDefaultRuntimeLibrary(const std::filesystem::path& kaiccExecutablePath) {
    std::error_code existsError;

    if (const char* envOverride = std::getenv("KAI_RUNTIME_LIB")) {
        const std::filesystem::path overridePath(envOverride);
        if (std::filesystem::exists(overridePath, existsError) && !existsError) {
            return overridePath;
        }
    }

    const std::filesystem::path executableDirectory = kaiccExecutablePath.parent_path();

    const std::filesystem::path packagedPath = executableDirectory / ".." / "lib" / "kai" / "libkai_runtime.a";
    if (std::filesystem::exists(packagedPath, existsError) && !existsError) {
        return packagedPath;
    }

    const std::filesystem::path flatPath = executableDirectory / "libkai_runtime.a";
    if (std::filesystem::exists(flatPath, existsError) && !existsError) {
        return flatPath;
    }

    return std::nullopt;
}

std::filesystem::path NativeLinker::currentExecutablePath() {
    std::error_code readError;
    std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", readError);
    if (readError) {
        return {};
    }
    return resolved;
}

bool NativeLinker::link(const std::string& compilerDriverPath, const std::filesystem::path& objectPath,
                         const std::filesystem::path& runtimeLibraryPath, const std::filesystem::path& outputPath,
                         std::ostream& err) {
    const std::string objectPathStr = objectPath.string();
    const std::string runtimeLibraryPathStr = runtimeLibraryPath.string();
    const std::string outputPathStr = outputPath.string();

    const std::array<llvm::StringRef, 5> args = {compilerDriverPath, objectPathStr, runtimeLibraryPathStr, "-o",
                                                  outputPathStr};

    std::string executionError;
    bool executionFailed = false;
    const int exitCode = llvm::sys::ExecuteAndWait(compilerDriverPath, args,
                                                    /*Env=*/std::nullopt, /*Redirects=*/{}, /*SecondsToWait=*/0,
                                                    /*MemoryLimit=*/0, &executionError, &executionFailed);

    if (executionFailed || exitCode != 0) {
        err << "kaicc: error: linking failed (" << compilerDriverPath << " exited with code " << exitCode << ")";
        if (!executionError.empty()) {
            err << ": " << executionError;
        }
        err << '\n';
        return false;
    }

    return true;
}

} // namespace kai::codegen
