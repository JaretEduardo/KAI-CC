#include "kai/cli/AstPrinter.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
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
#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::TokenKind;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::BinaryOperator;
using kai::ast::BindingKind;
using kai::ast::BlockPtr;
using kai::ast::BlockStmt;
using kai::ast::DeclPtr;
using kai::ast::Expr;
using kai::ast::ExprPtr;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::NamedTypeSyntax;
using kai::ast::Param;
using kai::ast::ReturnStmt;
using kai::ast::SourceFile;
using kai::ast::StmtPtr;
using kai::ast::TypeSyntaxPtr;
using kai::ast::UnaryExpr;
using kai::ast::UnaryOperator;
using kai::ast::VarDeclStmt;
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

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

// --- manually-constructed-tree rendering (Parser does not build these
// node kinds yet, so these tests hand-build a minimal SourceFile ->
// FunctionDecl -> BlockStmt wrapper around the statement/expression under
// test, then run it through the same public printAst() the Parser-driven
// tests above use.) ---

std::string printStatement(SourceManager& sm, FileId file, SourceSpan enclosingSpan, StmtPtr stmt) {
    std::vector<StmtPtr> statements;
    statements.push_back(std::move(stmt));
    BlockPtr body = std::make_unique<BlockStmt>(std::move(statements), enclosingSpan);

    DeclPtr fn = std::make_unique<FunctionDecl>(false, Identifier{enclosingSpan}, std::vector<Param>{}, nullptr,
                                                 std::move(body), enclosingSpan);

    std::vector<DeclPtr> decls;
    decls.push_back(std::move(fn));
    const SourceFile sourceFile(file, std::move(decls), enclosingSpan);

    std::ostringstream out;
    printAst(out, sm, sourceFile);
    return out.str();
}

std::string printExprAsStatement(SourceManager& sm, FileId file, SourceSpan enclosingSpan, ExprPtr expr) {
    const SourceSpan exprSpan = expr->span();
    StmtPtr stmt = std::make_unique<ExprStmt>(std::move(expr), exprSpan);
    return printStatement(sm, file, enclosingSpan, std::move(stmt));
}

void testVarDeclStmtImmutableWithoutTypeRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan initSpan = spanOf(file, 8, 10);
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    StmtPtr stmt =
        std::make_unique<VarDeclStmt>(BindingKind::Immutable, Identifier{nameSpan}, nullptr, std::move(initializer),
                                       fullSpan);

    const std::string text = printStatement(sm, file, fullSpan, std::move(stmt));

    KAI_CHECK(text.find("VarDeclStmt binding=Immutable name=\"x\"") != std::string::npos);
    KAI_CHECK(text.find("Type") == std::string::npos);
    KAI_CHECK(text.find("Initializer") != std::string::npos);
    KAI_CHECK(text.find("LiteralExpr kind=Integer lexeme=\"10\"") != std::string::npos);
}

void testVarDeclStmtTypedRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x: i32 = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan typeNameSpan = spanOf(file, 7, 10);
    const SourceSpan initSpan = spanOf(file, 13, 15);
    const SourceSpan fullSpan = spanOf(file, 0, 15);

    TypeSyntaxPtr type = std::make_unique<NamedTypeSyntax>(Identifier{typeNameSpan}, typeNameSpan);
    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    StmtPtr stmt = std::make_unique<VarDeclStmt>(BindingKind::Immutable, Identifier{nameSpan}, std::move(type),
                                                  std::move(initializer), fullSpan);

    const std::string text = printStatement(sm, file, fullSpan, std::move(stmt));

    const auto typeSectionPos = text.find("Type");
    const auto namedTypePos = text.find("NamedTypeSyntax name=\"i32\"");
    const auto initSectionPos = text.find("Initializer");

    KAI_CHECK(text.find("VarDeclStmt binding=Immutable name=\"x\"") != std::string::npos);
    KAI_CHECK(typeSectionPos != std::string::npos);
    KAI_CHECK(namedTypePos != std::string::npos);
    KAI_CHECK(initSectionPos != std::string::npos);
    KAI_CHECK(typeSectionPos < namedTypePos);
    KAI_CHECK(namedTypePos < initSectionPos);
}

void testVarDeclStmtMutableRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "mut x = 10");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan initSpan = spanOf(file, 8, 10);
    const SourceSpan fullSpan = spanOf(file, 0, 10);

    ExprPtr initializer = std::make_unique<LiteralExpr>(LiteralKind::Integer, initSpan);
    StmtPtr stmt = std::make_unique<VarDeclStmt>(BindingKind::Mutable, Identifier{nameSpan}, nullptr,
                                                  std::move(initializer), fullSpan);

    const std::string text = printStatement(sm, file, fullSpan, std::move(stmt));

    KAI_CHECK(text.find("VarDeclStmt binding=Mutable name=\"x\"") != std::string::npos);
}

void testReturnStmtBareRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "return");
    const SourceSpan span = spanOf(file, 0, 6);

    StmtPtr stmt = std::make_unique<ReturnStmt>(nullptr, span);
    const std::string text = printStatement(sm, file, span, std::move(stmt));

    KAI_CHECK(text.find("ReturnStmt") != std::string::npos);
    KAI_CHECK(text.find("Value") == std::string::npos);
}

void testReturnStmtValuedRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "return 42");
    const SourceSpan valueSpan = spanOf(file, 7, 9);
    const SourceSpan fullSpan = spanOf(file, 0, 9);

    ExprPtr value = std::make_unique<LiteralExpr>(LiteralKind::Integer, valueSpan);
    StmtPtr stmt = std::make_unique<ReturnStmt>(std::move(value), fullSpan);

    const std::string text = printStatement(sm, file, fullSpan, std::move(stmt));

    const auto returnPos = text.find("ReturnStmt");
    const auto valuePos = text.find("Value");
    const auto literalPos = text.find("LiteralExpr kind=Integer lexeme=\"42\"");

    KAI_CHECK(returnPos != std::string::npos);
    KAI_CHECK(valuePos != std::string::npos);
    KAI_CHECK(literalPos != std::string::npos);
    KAI_CHECK(returnPos < valuePos);
    KAI_CHECK(valuePos < literalPos);
}

void testUnaryExprNegateRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "-x");
    const SourceSpan opSpan = spanOf(file, 0, 1);
    const SourceSpan operandSpan = spanOf(file, 1, 2);
    const SourceSpan fullSpan = spanOf(file, 0, 2);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    ExprPtr expr = std::make_unique<UnaryExpr>(UnaryOperator::Negate, opSpan, std::move(operand), fullSpan);

    const std::string text = printExprAsStatement(sm, file, fullSpan, std::move(expr));

    const auto unaryPos = text.find("UnaryExpr op=Negate");
    const auto operandPos = text.find("Operand");
    const auto identPos = text.find("IdentifierExpr name=\"x\"");

    KAI_CHECK(unaryPos != std::string::npos);
    KAI_CHECK(operandPos != std::string::npos);
    KAI_CHECK(identPos != std::string::npos);
    KAI_CHECK(unaryPos < operandPos);
    KAI_CHECK(operandPos < identPos);
}

void testUnaryExprRefMutRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "&mut x");
    const SourceSpan opSpan = spanOf(file, 0, 4);
    const SourceSpan operandSpan = spanOf(file, 5, 6);
    const SourceSpan fullSpan = spanOf(file, 0, 6);

    ExprPtr operand = std::make_unique<IdentifierExpr>(Identifier{operandSpan}, operandSpan);
    ExprPtr expr = std::make_unique<UnaryExpr>(UnaryOperator::RefMut, opSpan, std::move(operand), fullSpan);

    const std::string text = printExprAsStatement(sm, file, fullSpan, std::move(expr));

    KAI_CHECK(text.find("UnaryExpr op=RefMut") != std::string::npos);
    KAI_CHECK(text.find("IdentifierExpr name=\"x\"") != std::string::npos);
}

void testBinaryExprRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "a+b");
    const SourceSpan leftSpan = spanOf(file, 0, 1);
    const SourceSpan opSpan = spanOf(file, 1, 2);
    const SourceSpan rightSpan = spanOf(file, 2, 3);
    const SourceSpan fullSpan = spanOf(file, 0, 3);

    ExprPtr left = std::make_unique<IdentifierExpr>(Identifier{leftSpan}, leftSpan);
    ExprPtr right = std::make_unique<IdentifierExpr>(Identifier{rightSpan}, rightSpan);
    ExprPtr expr = std::make_unique<BinaryExpr>(BinaryOperator::Add, opSpan, std::move(left), std::move(right),
                                                 fullSpan);

    const std::string text = printExprAsStatement(sm, file, fullSpan, std::move(expr));

    const auto binaryPos = text.find("BinaryExpr op=Add");
    const auto leftPos = text.find("Left");
    const auto leftIdentPos = text.find("IdentifierExpr name=\"a\"");
    const auto rightPos = text.find("Right");
    const auto rightIdentPos = text.find("IdentifierExpr name=\"b\"");

    KAI_CHECK(binaryPos != std::string::npos);
    KAI_CHECK(leftPos != std::string::npos);
    KAI_CHECK(leftIdentPos != std::string::npos);
    KAI_CHECK(rightPos != std::string::npos);
    KAI_CHECK(rightIdentPos != std::string::npos);
    KAI_CHECK(binaryPos < leftPos);
    KAI_CHECK(leftPos < leftIdentPos);
    KAI_CHECK(leftIdentPos < rightPos);
    KAI_CHECK(rightPos < rightIdentPos);
}

void testBinaryExprNestedPrecedenceRendering() {
    // Hand-built shape for `1 + 2 * 3` -> Add(1, Multiply(2, 3)).
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "1+2*3");

    const SourceSpan oneSpan = spanOf(file, 0, 1);
    const SourceSpan plusSpan = spanOf(file, 1, 2);
    const SourceSpan twoSpan = spanOf(file, 2, 3);
    const SourceSpan starSpan = spanOf(file, 3, 4);
    const SourceSpan threeSpan = spanOf(file, 4, 5);
    const SourceSpan mulSpan = spanOf(file, 2, 5);
    const SourceSpan addSpan = spanOf(file, 0, 5);

    ExprPtr two = std::make_unique<LiteralExpr>(LiteralKind::Integer, twoSpan);
    ExprPtr three = std::make_unique<LiteralExpr>(LiteralKind::Integer, threeSpan);
    ExprPtr mul = std::make_unique<BinaryExpr>(BinaryOperator::Multiply, starSpan, std::move(two), std::move(three),
                                                mulSpan);
    ExprPtr one = std::make_unique<LiteralExpr>(LiteralKind::Integer, oneSpan);
    ExprPtr add =
        std::make_unique<BinaryExpr>(BinaryOperator::Add, plusSpan, std::move(one), std::move(mul), addSpan);

    const std::string text = printExprAsStatement(sm, file, addSpan, std::move(add));

    const auto addPos = text.find("BinaryExpr op=Add");
    const auto mulPos = text.find("BinaryExpr op=Multiply");

    KAI_CHECK(addPos != std::string::npos);
    KAI_CHECK(mulPos != std::string::npos);
    KAI_CHECK(addPos < mulPos);
    KAI_CHECK(text.find("lexeme=\"1\"") != std::string::npos);
    KAI_CHECK(text.find("lexeme=\"2\"") != std::string::npos);
    KAI_CHECK(text.find("lexeme=\"3\"") != std::string::npos);
}

void testAssignmentExprRendering() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "a=b");
    const SourceSpan targetSpan = spanOf(file, 0, 1);
    const SourceSpan opSpan = spanOf(file, 1, 2);
    const SourceSpan valueSpan = spanOf(file, 2, 3);
    const SourceSpan fullSpan = spanOf(file, 0, 3);

    ExprPtr target = std::make_unique<IdentifierExpr>(Identifier{targetSpan}, targetSpan);
    ExprPtr value = std::make_unique<IdentifierExpr>(Identifier{valueSpan}, valueSpan);
    ExprPtr expr = std::make_unique<AssignmentExpr>(std::move(target), opSpan, std::move(value), fullSpan);

    const std::string text = printExprAsStatement(sm, file, fullSpan, std::move(expr));

    const auto assignPos = text.find("AssignmentExpr");
    const auto targetPos = text.find("Target");
    const auto targetIdentPos = text.find("IdentifierExpr name=\"a\"");
    const auto valuePos = text.find("Value");
    const auto valueIdentPos = text.find("IdentifierExpr name=\"b\"");

    KAI_CHECK(assignPos != std::string::npos);
    KAI_CHECK(targetPos != std::string::npos);
    KAI_CHECK(targetIdentPos != std::string::npos);
    KAI_CHECK(valuePos != std::string::npos);
    KAI_CHECK(valueIdentPos != std::string::npos);
    KAI_CHECK(assignPos < targetPos);
    KAI_CHECK(targetPos < targetIdentPos);
    KAI_CHECK(targetIdentPos < valuePos);
    KAI_CHECK(valuePos < valueIdentPos);
    // operatorSpan/source offsets are never printed.
    KAI_CHECK(text.find("operatorSpan") == std::string::npos);
}

void testAssignmentExprRightAssociativeChainRendering() {
    // Hand-built shape for `a = b = c` -> Assignment(a, Assignment(b, c)).
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "a=b=c");

    const SourceSpan aSpan = spanOf(file, 0, 1);
    const SourceSpan eq1Span = spanOf(file, 1, 2);
    const SourceSpan bSpan = spanOf(file, 2, 3);
    const SourceSpan eq2Span = spanOf(file, 3, 4);
    const SourceSpan cSpan = spanOf(file, 4, 5);
    const SourceSpan innerSpan = spanOf(file, 2, 5);
    const SourceSpan outerSpan = spanOf(file, 0, 5);

    ExprPtr b = std::make_unique<IdentifierExpr>(Identifier{bSpan}, bSpan);
    ExprPtr c = std::make_unique<IdentifierExpr>(Identifier{cSpan}, cSpan);
    ExprPtr inner = std::make_unique<AssignmentExpr>(std::move(b), eq2Span, std::move(c), innerSpan);
    ExprPtr a = std::make_unique<IdentifierExpr>(Identifier{aSpan}, aSpan);
    ExprPtr outer = std::make_unique<AssignmentExpr>(std::move(a), eq1Span, std::move(inner), outerSpan);

    const std::string text = printExprAsStatement(sm, file, outerSpan, std::move(outer));

    const auto outerPos = text.find("AssignmentExpr");
    const auto innerPos = text.find("AssignmentExpr", outerPos + 1);

    KAI_CHECK(outerPos != std::string::npos);
    KAI_CHECK(innerPos != std::string::npos);
    KAI_CHECK(innerPos > outerPos);
    KAI_CHECK(text.find("IdentifierExpr name=\"a\"") != std::string::npos);
    KAI_CHECK(text.find("IdentifierExpr name=\"b\"") != std::string::npos);
    KAI_CHECK(text.find("IdentifierExpr name=\"c\"") != std::string::npos);
}

void testVarDeclStmtWithBinaryInitializerExactShape() {
    // "let x: i32 = 1 + 2"
    //  0123456789012345678
    //            1111111 1
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x: i32 = 1 + 2");
    const SourceSpan nameSpan = spanOf(file, 4, 5);
    const SourceSpan typeNameSpan = spanOf(file, 7, 10);
    const SourceSpan leftSpan = spanOf(file, 13, 14);
    const SourceSpan opSpan = spanOf(file, 15, 16);
    const SourceSpan rightSpan = spanOf(file, 17, 18);
    const SourceSpan binarySpan = spanOf(file, 13, 18);
    const SourceSpan fullSpan = spanOf(file, 0, 18);

    TypeSyntaxPtr type = std::make_unique<NamedTypeSyntax>(Identifier{typeNameSpan}, typeNameSpan);
    ExprPtr left = std::make_unique<LiteralExpr>(LiteralKind::Integer, leftSpan);
    ExprPtr right = std::make_unique<LiteralExpr>(LiteralKind::Integer, rightSpan);
    ExprPtr initializer =
        std::make_unique<BinaryExpr>(BinaryOperator::Add, opSpan, std::move(left), std::move(right), binarySpan);
    StmtPtr stmt = std::make_unique<VarDeclStmt>(BindingKind::Immutable, Identifier{nameSpan}, std::move(type),
                                                  std::move(initializer), fullSpan);

    const std::string text = printStatement(sm, file, fullSpan, std::move(stmt));

    // Depth accounting: SourceFile(0) -> FunctionDecl(1) -> BlockStmt(2)
    // -> VarDeclStmt(3) -> Type/Initializer(4) -> NamedTypeSyntax /
    // BinaryExpr(5) -> Left/Right(6) -> LiteralExpr(7).
    const std::string expected = "      VarDeclStmt binding=Immutable name=\"x\"\n"
                                  "        Type\n"
                                  "          NamedTypeSyntax name=\"i32\"\n"
                                  "        Initializer\n"
                                  "          BinaryExpr op=Add\n"
                                  "            Left\n"
                                  "              LiteralExpr kind=Integer lexeme=\"1\"\n"
                                  "            Right\n"
                                  "              LiteralExpr kind=Integer lexeme=\"2\"\n";

    KAI_CHECK(text.find(expected) != std::string::npos);
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

    testVarDeclStmtImmutableWithoutTypeRendering();
    testVarDeclStmtTypedRendering();
    testVarDeclStmtMutableRendering();
    testReturnStmtBareRendering();
    testReturnStmtValuedRendering();
    testUnaryExprNegateRendering();
    testUnaryExprRefMutRendering();
    testBinaryExprRendering();
    testBinaryExprNestedPrecedenceRendering();
    testAssignmentExprRendering();
    testAssignmentExprRightAssociativeChainRendering();
    testVarDeclStmtWithBinaryInitializerExactShape();

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
