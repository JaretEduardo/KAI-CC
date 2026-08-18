#include "kai/cli/TokenPrinter.hpp"

#include "kai/lexer/Lexer.hpp"
#include "kai/lexer/Token.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using kai::FileId;
using kai::Lexer;
using kai::SourceManager;
using kai::Token;
using kai::TokenKind;
using kai::cli::escapeLexeme;
using kai::cli::printToken;
using kai::cli::runTokensCommand;

namespace {

// --- escapeLexeme ---

void testEscapeLexemeSpecialChars() {
    KAI_CHECK(escapeLexeme("\n") == "\\n");
    KAI_CHECK(escapeLexeme("\r") == "\\r");
    KAI_CHECK(escapeLexeme("\t") == "\\t");
    KAI_CHECK(escapeLexeme("\\") == "\\\\");
    KAI_CHECK(escapeLexeme("\"") == "\\\"");
}

void testEscapeLexemeControlAndDelBytes() {
    const std::string nul(1, '\0');
    KAI_CHECK(escapeLexeme(nul) == "\\x00");

    const std::string soh(1, static_cast<char>(0x01));
    KAI_CHECK(escapeLexeme(soh) == "\\x01");

    const std::string del(1, static_cast<char>(0x7F));
    KAI_CHECK(escapeLexeme(del) == "\\x7F");
}

void testEscapeLexemeHighBytes() {
    const std::string highByte(1, static_cast<char>(0x80));
    KAI_CHECK(escapeLexeme(highByte) == "\\x80");

    const std::string ffByte(1, static_cast<char>(0xFF));
    KAI_CHECK(escapeLexeme(ffByte) == "\\xFF");
}

void testEscapeLexemeMultiByteUtf8IsRenderedByteByByte() {
    // U+00E1 ("á") encoded as UTF-8: 0xC3 0xA1. Must not be treated as a
    // valid codepoint or passed through raw; each byte is escaped alone.
    const std::string bytes = {static_cast<char>(0xC3), static_cast<char>(0xA1)};
    KAI_CHECK(escapeLexeme(bytes) == "\\xC3\\xA1");
}

void testEscapeLexemePrintableAsciiUnchanged() {
    KAI_CHECK(escapeLexeme("Hello from KAI 123!") == "Hello from KAI 123!");
    KAI_CHECK(escapeLexeme("") == "");
}

// --- printToken ---

std::vector<std::string> printAllTokens(SourceManager& sm, FileId file) {
    Lexer lexer(sm, file);
    std::ostringstream out;

    while (true) {
        const Token token = lexer.nextToken();
        printToken(out, sm, token);
        if (token.kind() == TokenKind::EndOfFile) {
            break;
        }
    }

    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(out.str());
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

void testPrintTokenKindAndLexeme() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn");

    Lexer lexer(sm, file);
    const Token token = lexer.nextToken();

    std::ostringstream out;
    printToken(out, sm, token);
    const std::string line = out.str();

    // Exact column widths are not part of the interface (only that kind,
    // escaped lexeme, and line:column all appear, in that order).
    KAI_CHECK(line.starts_with("KwFn"));
    KAI_CHECK(line.find("\"fn\"") != std::string::npos);
    KAI_CHECK(line.find("\"fn\"") < line.find("1:1"));
    KAI_CHECK(line.ends_with("1:1\n"));
}

void testPrintTokenStringLiteralKeepsQuotesVisible() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "\"Hello\"");

    Lexer lexer(sm, file);
    const Token token = lexer.nextToken();
    KAI_CHECK(token.kind() == TokenKind::StringLiteral);

    std::ostringstream out;
    printToken(out, sm, token);
    const std::string line = out.str();

    // The raw source lexeme is `"Hello"` (quotes included); escapeLexeme
    // escapes the embedded quote bytes, and printToken wraps the result
    // in an outer pair of quotes for display: "\"Hello\"".
    KAI_CHECK(line.starts_with("StringLiteral"));
    KAI_CHECK(line.find("\"\\\"Hello\\\"\"") != std::string::npos);
}

void testPrintTokenNewlineRendersOnOneLine() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn\n");

    const auto lines = printAllTokens(sm, file);

    KAI_CHECK(lines.size() == 3); // KwFn, Newline, EndOfFile
    KAI_CHECK(lines[1].starts_with("Newline"));
    KAI_CHECK(lines[1].find("\\n") != std::string::npos);
    // The escaped "\n" must not have produced a second physical output line.
    KAI_CHECK(lines[1].find('\n') == std::string::npos);
}

void testPrintTokenLineColumn() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() {\n    x\n}");

    const auto lines = printAllTokens(sm, file);

    KAI_CHECK(lines[0].find("1:1") != std::string::npos);   // KwFn
    KAI_CHECK(lines[1].find("1:4") != std::string::npos);   // Identifier "main"
    KAI_CHECK(lines[6].find("2:5") != std::string::npos);   // Identifier "x"
}

void testPrintTokenInvalidIsVisible() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "$");

    Lexer lexer(sm, file);
    const Token token = lexer.nextToken();
    KAI_CHECK(token.kind() == TokenKind::Invalid);

    std::ostringstream out;
    printToken(out, sm, token);
    const std::string line = out.str();

    KAI_CHECK(line.starts_with("Invalid"));
    KAI_CHECK(line.find("\"$\"") != std::string::npos);
}

void testPrintTokenEofHasNoLexeme() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "");

    Lexer lexer(sm, file);
    const Token token = lexer.nextToken();
    KAI_CHECK(token.kind() == TokenKind::EndOfFile);

    std::ostringstream out;
    printToken(out, sm, token);

    const std::string line = out.str();
    KAI_CHECK(line.starts_with("EndOfFile"));
    KAI_CHECK(line.find('"') == std::string::npos);
    KAI_CHECK(line.find("1:1") != std::string::npos);
}

// --- runTokensCommand ---

std::filesystem::path writeTempFile(const std::string& name, const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file << contents;
    return path;
}

void testRunTokensCommandValidSourceReturnsZeroAndPrintsAllTokens() {
    const std::filesystem::path path = writeTempFile("kaicc_cli_test_valid.kai", "fn main() {}\n");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runTokensCommand(sm, path, out, err);

    KAI_CHECK(code == 0);
    KAI_CHECK(err.str().empty());

    const std::string text = out.str();
    KAI_CHECK(text.find("KwFn") != std::string::npos);
    KAI_CHECK(text.find("EndOfFile") != std::string::npos);

    std::filesystem::remove(path);
}

void testRunTokensCommandInvalidTokenReturnsThreeButPrintsAllTokens() {
    const std::filesystem::path path = writeTempFile("kaicc_cli_test_invalid.kai", "fn $ main\n");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runTokensCommand(sm, path, out, err);

    KAI_CHECK(code == 3);

    const std::string text = out.str();
    KAI_CHECK(text.find("KwFn") != std::string::npos);
    KAI_CHECK(text.find("Invalid") != std::string::npos);
    KAI_CHECK(text.find("Identifier") != std::string::npos);
    KAI_CHECK(text.find("EndOfFile") != std::string::npos);

    std::filesystem::remove(path);
}

void testRunTokensCommandMissingFileReturnsTwo() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "kaicc_cli_test_does_not_exist.kai";
    std::filesystem::remove(path);

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runTokensCommand(sm, path, out, err);

    KAI_CHECK(code == 2);
    KAI_CHECK(out.str().empty());
    KAI_CHECK(!err.str().empty());
    KAI_CHECK(err.str().find(path.string()) != std::string::npos);
}

} // namespace

int main() {
    testEscapeLexemeSpecialChars();
    testEscapeLexemeControlAndDelBytes();
    testEscapeLexemeHighBytes();
    testEscapeLexemeMultiByteUtf8IsRenderedByteByByte();
    testEscapeLexemePrintableAsciiUnchanged();

    testPrintTokenKindAndLexeme();
    testPrintTokenStringLiteralKeepsQuotesVisible();
    testPrintTokenNewlineRendersOnOneLine();
    testPrintTokenLineColumn();
    testPrintTokenInvalidIsVisible();
    testPrintTokenEofHasNoLexeme();

    testRunTokensCommandValidSourceReturnsZeroAndPrintsAllTokens();
    testRunTokensCommandInvalidTokenReturnsThreeButPrintsAllTokens();
    testRunTokensCommandMissingFileReturnsTwo();

    return kai::test::failureCount == 0 ? 0 : 1;
}
