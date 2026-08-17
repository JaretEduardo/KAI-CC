#include "kai/cli/TokenPrinter.hpp"

#include "kai/lexer/Lexer.hpp"
#include "kai/lexer/TokenKind.hpp"

#include <array>
#include <iomanip>

namespace kai::cli {

namespace {

constexpr int kKindColumnWidth = 16;
constexpr int kLexemeColumnWidth = 24;

char toHexDigit(unsigned char nibble) noexcept {
    constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    return kDigits[nibble & 0xF];
}

void appendHexByte(std::string& out, unsigned char byte) {
    out += "\\x";
    out += toHexDigit(static_cast<unsigned char>(byte >> 4));
    out += toHexDigit(byte);
}

} // namespace

std::string escapeLexeme(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    for (unsigned char c : text) {
        switch (c) {
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            default:
                if (c >= 0x20 && c <= 0x7E) {
                    result += static_cast<char>(c);
                } else {
                    appendHexByte(result, c);
                }
                break;
        }
    }

    return result;
}

void printToken(std::ostream& out, const SourceManager& sources, Token token) {
    const SourceManager::LineColumn where = sources.lineColumn(token.span().begin());

    out << std::left << std::setw(kKindColumnWidth) << tokenKindName(token.kind());

    if (token.kind() == TokenKind::EndOfFile) {
        out << std::left << std::setw(kLexemeColumnWidth) << "";
    } else {
        const std::string quoted = '"' + escapeLexeme(sources.text(token.span())) + '"';
        out << std::left << std::setw(kLexemeColumnWidth) << quoted;
    }

    out << where.line << ':' << where.column << '\n';
}

int runTokensCommand(SourceManager& sources, const std::filesystem::path& path, std::ostream& out,
                      std::ostream& err) {
    const auto loaded = sources.loadFile(path);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << path.string() << "': " << loaded.error().message() << '\n';
        return 2;
    }

    Lexer lexer(sources, *loaded);
    bool sawInvalid = false;

    while (true) {
        const Token token = lexer.nextToken();
        printToken(out, sources, token);

        if (token.kind() == TokenKind::Invalid) {
            sawInvalid = true;
        }

        if (token.kind() == TokenKind::EndOfFile) {
            break;
        }
    }

    return sawInvalid ? 3 : 0;
}

} // namespace kai::cli
