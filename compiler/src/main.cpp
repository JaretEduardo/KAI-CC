#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view KAI_VERSION = "0.1.0-dev";

void printVersion() {
    std::cout << "KAI-CC " << KAI_VERSION << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        printVersion();
        return 0;
    }

    std::cout << "KAI-CC compiler\n";
    std::cout << "Usage: kaicc [options] <file.kai>\n";
    std::cout << "Try 'kaicc --version'\n";

    return 0;
}