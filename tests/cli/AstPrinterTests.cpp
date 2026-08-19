#include "kai/cli/AstPrinter.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/TokenPrinter.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/parser/ParseError.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using kai::FileId;
using kai::SourceManager;
using kai::TokenKind;
using kai::ast::SourceFile;
using kai::cli::escapeLexeme;
using kai::cli::formatParseError;
using kai::cli::printAst;
using kai::cli::runAstCommand;
using kai::parser::ParseError;
using kai::parser::ParseErrorKind;
using kai::parser::Parser;

namespace {

// --- printAst ---

std::string printSource(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    KAI_CHECK(parsed.has_value());
    if (!parsed) {
        return {};
    }

    std::ostringstream out;
    printAst(out, sm, *parsed);
    return out.str();
}

void testHelloWorldExactTree() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    print(\"Hello from KAI\")\n}\n");

    const std::string escapedLexeme = escapeLexeme("\"Hello from KAI\"");
    const std::string expected = "SourceFile\n"
                                  "  FunctionDecl name=\"main\" public=false\n"
                                  "    BlockStmt\n"
                                  "      ExprStmt\n"
                                  "        CallExpr\n"
                                  "          IdentifierExpr name=\"print\"\n"
                                  "          LiteralExpr kind=String lexeme=\"" +
                                  escapedLexeme + "\"\n";

    KAI_CHECK(text == expected);
}

void testFunctionNameRendering() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn add() {}\n");
    KAI_CHECK(text.find("FunctionDecl name=\"add\"") != std::string::npos);
}

void testPublicFlagFalseByDefault() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {}\n");
    KAI_CHECK(text.find("public=false") != std::string::npos);
}

void testPublicFlagTrueForPubFn() {
    SourceManager sm;
    const std::string text = printSource(sm, "pub fn main() {}\n");
    KAI_CHECK(text.find("public=true") != std::string::npos);
}

void testParametersRenderedInOrder() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn add(a: i32, b: i32) {}\n");

    const auto parametersPos = text.find("Parameters");
    const auto paramAPos = text.find("Param name=\"a\"");
    const auto typeAPos = text.find("NamedTypeSyntax name=\"i32\"");
    const auto paramBPos = text.find("Param name=\"b\"");

    KAI_CHECK(parametersPos != std::string::npos);
    KAI_CHECK(paramAPos != std::string::npos);
    KAI_CHECK(typeAPos != std::string::npos);
    KAI_CHECK(paramBPos != std::string::npos);
    KAI_CHECK(parametersPos < paramAPos);
    KAI_CHECK(paramAPos < typeAPos);
    KAI_CHECK(typeAPos < paramBPos);
}

void testEmptyParametersOmitsSection() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {}\n");
    KAI_CHECK(text.find("Parameters") == std::string::npos);
}

void testReturnTypePresent() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn add() -> i32 {}\n");

    const auto returnTypePos = text.find("ReturnType");
    const auto namedTypePos = text.find("NamedTypeSyntax name=\"i32\"");

    KAI_CHECK(returnTypePos != std::string::npos);
    KAI_CHECK(namedTypePos != std::string::npos);
    KAI_CHECK(returnTypePos < namedTypePos);
}

void testReturnTypeAbsentOmitsSection() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {}\n");
    KAI_CHECK(text.find("ReturnType") == std::string::npos);
}

void testEmptyBlockStmtRemainsVisible() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {}\n");
    KAI_CHECK(text.find("BlockStmt") != std::string::npos);
    // Nothing should be nested beneath it: BlockStmt is the last line.
    KAI_CHECK(text.ends_with("BlockStmt\n"));
}

void testIdentifierExprRendering() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    foo(x)\n}\n");
    KAI_CHECK(text.find("IdentifierExpr name=\"x\"") != std::string::npos);
}

void testLiteralExprStringKindAndLexeme() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    \"hi\"\n}\n");
    KAI_CHECK(text.find("LiteralExpr kind=String lexeme=\"\\\"hi\\\"\"") != std::string::npos);
}

void testLiteralExprIntegerKindAndLexeme() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    42\n}\n");
    KAI_CHECK(text.find("LiteralExpr kind=Integer lexeme=\"42\"") != std::string::npos);
}

void testCallExprArgumentOrdering() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    foo(1, 2, 3)\n}\n");

    const auto pos1 = text.find("lexeme=\"1\"");
    const auto pos2 = text.find("lexeme=\"2\"");
    const auto pos3 = text.find("lexeme=\"3\"");

    KAI_CHECK(pos1 != std::string::npos);
    KAI_CHECK(pos2 != std::string::npos);
    KAI_CHECK(pos3 != std::string::npos);
    KAI_CHECK(pos1 < pos2);
    KAI_CHECK(pos2 < pos3);
}

void testParenExprRemainsVisible() {
    SourceManager sm;
    const std::string text = printSource(sm, "fn main() {\n    (foo())\n}\n");

    const auto parenPos = text.find("ParenExpr");
    const auto callPos = text.find("CallExpr");

    KAI_CHECK(parenPos != std::string::npos);
    KAI_CHECK(callPos != std::string::npos);
    KAI_CHECK(parenPos < callPos);
}

// --- formatParseError ---

ParseError parseError(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    KAI_CHECK(!parsed.has_value());
    if (parsed.has_value()) {
        return ParseError{};
    }
    return parsed.error();
}

void testUnexpectedTokenWithoutExpected() {
    SourceManager sm;
    const ParseError error = parseError(sm, "123\n");

    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::IntegerLiteral);
    KAI_CHECK(!error.expected.has_value());

    const std::string message = formatParseError(sm, error);
    KAI_CHECK(message == "kaicc: parse error at 1:1: unexpected IntegerLiteral");
}

void testUnexpectedTokenWithExpected() {
    SourceManager sm;
    const ParseError error = parseError(sm, "fn main(a: i32 {}\n");

    KAI_CHECK(error.kind == ParseErrorKind::UnexpectedToken);
    KAI_CHECK(error.actual == TokenKind::LeftBrace);
    KAI_CHECK(error.expected.has_value());
    KAI_CHECK(*error.expected == TokenKind::RightParen);

    const std::string message = formatParseError(sm, error);
    KAI_CHECK(message.find("expected RightParen, got LeftBrace") != std::string::npos);
}

void testUnsupportedSyntax() {
    SourceManager sm;
    const ParseError error = parseError(sm, "struct Foo {}\n");

    KAI_CHECK(error.kind == ParseErrorKind::UnsupportedSyntax);
    KAI_CHECK(error.actual == TokenKind::KwStruct);

    const std::string message = formatParseError(sm, error);
    KAI_CHECK(message == "kaicc: parse error at 1:1: unsupported syntax starting with KwStruct");
}

void testInvalidTokenIncludesEscapedLexeme() {
    SourceManager sm;
    const ParseError error = parseError(sm, "$\n");

    KAI_CHECK(error.kind == ParseErrorKind::InvalidToken);
    KAI_CHECK(error.actual == TokenKind::Invalid);

    const std::string message = formatParseError(sm, error);
    KAI_CHECK(message == "kaicc: parse error at 1:1: invalid token \"$\"");
}

void testParseErrorLineColumnReflectsPosition() {
    SourceManager sm;
    const ParseError error = parseError(sm, "fn main() {\n    struct\n}\n");

    const std::string message = formatParseError(sm, error);
    KAI_CHECK(message.find("at 2:5:") != std::string::npos);
}

// --- runAstCommand ---

std::filesystem::path writeTempFile(const std::string& name, const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file << contents;
    return path;
}

void testRunAstCommandValidFileReturnsZeroAndPrintsTree() {
    const std::filesystem::path path = writeTempFile("kaicc_cli_ast_test_valid.kai", "fn main() {}\n");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runAstCommand(sm, path, out, err);

    KAI_CHECK(code == 0);
    KAI_CHECK(err.str().empty());
    KAI_CHECK(out.str().find("SourceFile") != std::string::npos);
    KAI_CHECK(out.str().find("FunctionDecl name=\"main\"") != std::string::npos);

    std::filesystem::remove(path);
}

void testRunAstCommandParseFailureReturnsFourAndWritesStderr() {
    const std::filesystem::path path = writeTempFile("kaicc_cli_ast_test_parse_error.kai", "struct Foo {}\n");

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runAstCommand(sm, path, out, err);

    KAI_CHECK(code == 4);
    KAI_CHECK(out.str().empty());
    KAI_CHECK(err.str().find("unsupported syntax starting with KwStruct") != std::string::npos);

    std::filesystem::remove(path);
}

void testRunAstCommandMissingFileReturnsTwo() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kaicc_cli_ast_test_does_not_exist.kai";
    std::filesystem::remove(path);

    SourceManager sm;
    std::ostringstream out;
    std::ostringstream err;
    const int code = runAstCommand(sm, path, out, err);

    KAI_CHECK(code == 2);
    KAI_CHECK(out.str().empty());
    KAI_CHECK(!err.str().empty());
    KAI_CHECK(err.str().find(path.string()) != std::string::npos);
}

} // namespace

int main() {
    testHelloWorldExactTree();
    testFunctionNameRendering();
    testPublicFlagFalseByDefault();
    testPublicFlagTrueForPubFn();
    testParametersRenderedInOrder();
    testEmptyParametersOmitsSection();
    testReturnTypePresent();
    testReturnTypeAbsentOmitsSection();
    testEmptyBlockStmtRemainsVisible();
    testIdentifierExprRendering();
    testLiteralExprStringKindAndLexeme();
    testLiteralExprIntegerKindAndLexeme();
    testCallExprArgumentOrdering();
    testParenExprRemainsVisible();

    testUnexpectedTokenWithoutExpected();
    testUnexpectedTokenWithExpected();
    testUnsupportedSyntax();
    testInvalidTokenIncludesEscapedLexeme();
    testParseErrorLineColumnReflectsPosition();

    testRunAstCommandValidFileReturnsZeroAndPrintsTree();
    testRunAstCommandParseFailureReturnsFourAndWritesStderr();
    testRunAstCommandMissingFileReturnsTwo();

    return kai::test::failureCount == 0 ? 0 : 1;
}
