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
    std::cout << "Usage: kaicc [options] <file.kai>\n";
    std::cout << "Try 'kaicc --version'\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        printVersion();
        return 0;
    }

    if (argc == 1) {
        printUsage();
        return 0;
    }

    const std::string_view firstArg(argv[1]);

    if (firstArg == "--tokens") {
        if (argc != 3) {
            std::cerr << "kaicc: error: --tokens requires exactly one file argument\n";
            std::cerr << "Usage: kaicc --tokens <file.kai>\n";
            return 1;
        }

        kai::SourceManager sources;
        return kai::cli::runTokensCommand(sources, argv[2], std::cout, std::cerr);
    }

    if (firstArg.starts_with("--")) {
        std::cerr << "kaicc: error: unsupported option '" << firstArg << "'\n";
        std::cerr << "Usage: kaicc [options] <file.kai>\n";
        return 1;
    }

    // A source file was given, but full compilation is not implemented yet
    // (KAI-CC is still Phase 0/1 frontend work). Use `--tokens` to inspect
    // how a file lexes in the meantime.
    std::cerr << "kaicc: error: compilation is not implemented yet\n";
    std::cerr << "Usage: kaicc [options] <file.kai>\n";
    return 1;
}
