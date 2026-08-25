#include "kai/cli/AstPrinter.hpp"
#include "kai/cli/CompileCommand.hpp"
#include "kai/cli/InspectCommand.hpp"
#include "kai/cli/SemanticQueryCommand.hpp"
#include "kai/cli/TokenPrinter.hpp"
#include "kai/source/SourceManager.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
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
    std::cout << "  kaicc inspect <file.kai> --json\n";
    std::cout << "  kaicc definition <file.kai> --line N --column M --json\n";
    std::cout << "  kaicc references <file.kai> --line N --column M --json\n";
    std::cout << "Try 'kaicc --version'\n";
}

// SEMANTIC INSPECTION MILESTONE 2: `--line`/`--column` must be positive
// (1-indexed) integers - `line == 0` or `column == 0`, a malformed
// integer, or trailing garbage are all command errors (M2 spec §18),
// never silently coerced to some default. `std::from_chars` (rather than
// `std::stoul`) never throws and requires the ENTIRE argument to be
// consumed as digits - "12x" is rejected, not silently truncated to 12.
std::optional<std::uint32_t> parsePositiveLineOrColumn(std::string_view text) {
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if (value == 0) {
        return std::nullopt;
    }
    return value;
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

    // SEMANTIC INSPECTION MILESTONE 1: `kaicc inspect <input.kai> --json`
    // - a distinct mode from ordinary compilation (see InspectCommand.hpp):
    // no LLVM/object/link stage runs. `--json` is REQUIRED for M1 (spec
    // §18) - this keeps the semantic-tooling surface explicit rather than
    // silently defaulting to some future human-readable format.
    if (firstArg == "inspect") {
        if (argc != 4 || std::string_view(argv[3]) != "--json") {
            std::cerr << "kaicc: error: expected 'kaicc inspect <file.kai> --json'\n";
            std::cerr << "Usage: kaicc inspect <file.kai> --json\n";
            return 1;
        }

        kai::SourceManager sources;
        return kai::cli::runInspectCommand(sources, argv[2], std::cout, std::cerr);
    }

    // SEMANTIC INSPECTION MILESTONE 2: `kaicc definition <file.kai>
    // --line N --column M --json` / `kaicc references <file.kai> --line N
    // --column M --json` - position-based semantic queries (see
    // SemanticQueryCommand.hpp). Both subcommands share this one fixed-
    // shape argv parse; only which SemanticQueryKind gets passed through
    // differs. `--line`/`--column`/`--json` are all REQUIRED, and
    // line/column must each be a valid positive integer - any other shape
    // is a usage error (M2 spec §18), never a silent default/coercion.
    if (firstArg == "definition" || firstArg == "references") {
        bool usageOk = argc == 8 && std::string_view(argv[3]) == "--line" &&
                        std::string_view(argv[5]) == "--column" && std::string_view(argv[7]) == "--json";

        std::optional<std::uint32_t> line;
        std::optional<std::uint32_t> column;
        if (usageOk) {
            line = parsePositiveLineOrColumn(argv[4]);
            column = parsePositiveLineOrColumn(argv[6]);
            usageOk = line.has_value() && column.has_value();
        }

        if (!usageOk) {
            std::cerr << "kaicc: error: expected 'kaicc " << firstArg
                       << " <file.kai> --line N --column M --json' (N and M must be positive integers)\n";
            std::cerr << "Usage: kaicc " << firstArg << " <file.kai> --line N --column M --json\n";
            return 1;
        }

        kai::SourceManager sources;
        const kai::cli::SemanticQueryKind kind =
            firstArg == "definition" ? kai::cli::SemanticQueryKind::Definition : kai::cli::SemanticQueryKind::References;
        return kai::cli::runSemanticQueryCommand(kind, sources, argv[2], *line, *column, std::cout, std::cerr);
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
