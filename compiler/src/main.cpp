#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/CompileCommand.hpp"
#include "kai/cli/TokenPrinter.hpp"
#include "kai/source/SourceManager.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view KAI_VERSION = "0.1.0-dev";

void printVersion() {
    std::cout << "KAI-CC " << KAI_VERSION << '\n';
}

void printUsage() {
    std::cout << "KAI-CC compiler\n";
    std::cout << "Usage:\n";
    std::cout << "  kaicc --version\n";
    std::cout << "  kaicc --help\n";
    std::cout << "  kaicc --tokens <file.kai>\n";
    std::cout << "  kaicc --ast <file.kai>\n";
    std::cout << "  kaicc <file.kai> -o <output>\n";
    std::cout << "Try 'kaicc --version'\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        printUsage();
        return 0;
    }

    const std::string_view firstArg(argv[1]);

    if (firstArg == "--version") {
        printVersion();
        return 0;
    }

    if (firstArg == "--help") {
        printUsage();
        return 0;
    }

    if (firstArg == "--tokens") {
        if (argc != 3) {
            std::cerr << "kaicc: error: --tokens requires exactly one file argument\n";
            std::cerr << "Usage: kaicc --tokens <file.kai>\n";
            return 1;
        }

        kai::SourceManager sources;
        return kai::cli::runTokensCommand(sources, argv[2], std::cout, std::cerr);
    }

    if (firstArg == "--ast") {
        if (argc != 3) {
            std::cerr << "kaicc: error: --ast requires exactly one file argument\n";
            std::cerr << "Usage: kaicc --ast <file.kai>\n";
            return 1;
        }

        kai::SourceManager sources;
        return kai::cli::runAstCommand(sources, argv[2], std::cout, std::cerr);
    }

    if (firstArg.starts_with("--")) {
        std::cerr << "kaicc: error: unsupported option '" << firstArg << "'\n";
        std::cerr << "Usage: kaicc [options] <file.kai>\n";
        return 1;
    }

    // LLVM CODEGEN MILESTONE 7: `kaicc <input.kai> -o <output>` - the real
    // end-to-end native compilation pipeline (see CompileCommand.hpp).
    // Every other shape (missing `-o`, extra arguments) is a usage error,
    // never a silent fallback to some other mode.
    if (argc == 4 && std::string_view(argv[2]) == "-o") {
        kai::SourceManager sources;
        return kai::cli::runCompileCommand(sources, argv[1], argv[3], std::cerr);
    }

    std::cerr << "kaicc: error: expected 'kaicc <file.kai> -o <output>'\n";
    std::cerr << "Usage: kaicc [options] <file.kai>\n";
    return 1;
}
