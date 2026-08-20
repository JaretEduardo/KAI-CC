#include "kai/cli/AstPrinter.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/cli/TokenPrinter.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/parser/Parser.hpp"

#include <optional>
#include <sstream>
#include <string_view>

namespace kai::cli {

namespace {

void writeIndent(std::ostream& out, int depth) {
    for (int i = 0; i < depth; ++i) {
        out << "  ";
    }
}

// Not named `quoted`: std::quoted (<iomanip>, pulled in transitively via
// <sstream>) is found by ADL for any std::string/std::string_view
// argument and would silently outcompete a same-named overload here,
// since std::quoted binds its argument by reference (no
// string->string_view conversion needed) while this function requires
// one - std::quoted would win overload resolution. Worse, std::quoted's
// stream inserter re-escapes '"' and '\' itself, silently double-escaping
// any text (like an already-escaped literal lexeme) that contains them.
std::string wrapInQuotes(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result += '"';
    result += text;
    result += '"';
    return result;
}

// No `default:` case on purpose, matching tokenKindName()'s idiom: adding a
// LiteralKind enumerator without a corresponding name here should trigger
// -Wswitch.
std::string_view literalKindName(ast::LiteralKind kind) {
    switch (kind) {
        case ast::LiteralKind::Integer:
            return "Integer";
        case ast::LiteralKind::Float:
            return "Float";
        case ast::LiteralKind::String:
            return "String";
        case ast::LiteralKind::Char:
            return "Char";
        case ast::LiteralKind::Bool:
            return "Bool";
    }

    return "Unknown";
}

// No `default:` case, same idiom.
std::string_view bindingKindName(ast::BindingKind kind) {
    switch (kind) {
        case ast::BindingKind::Immutable:
            return "Immutable";
        case ast::BindingKind::Mutable:
            return "Mutable";
    }

    return "Unknown";
}

// No `default:` case, same idiom.
std::string_view unaryOperatorName(ast::UnaryOperator op) {
    switch (op) {
        case ast::UnaryOperator::Negate:
            return "Negate";
        case ast::UnaryOperator::Not:
            return "Not";
        case ast::UnaryOperator::Ref:
            return "Ref";
        case ast::UnaryOperator::RefMut:
            return "RefMut";
    }

    return "Unknown";
}

// No `default:` case, same idiom.
std::string_view binaryOperatorName(ast::BinaryOperator op) {
    switch (op) {
        case ast::BinaryOperator::Or:
            return "Or";
        case ast::BinaryOperator::And:
            return "And";
        case ast::BinaryOperator::Equal:
            return "Equal";
        case ast::BinaryOperator::NotEqual:
            return "NotEqual";
        case ast::BinaryOperator::Less:
            return "Less";
        case ast::BinaryOperator::LessEqual:
            return "LessEqual";
        case ast::BinaryOperator::Greater:
            return "Greater";
        case ast::BinaryOperator::GreaterEqual:
            return "GreaterEqual";
        case ast::BinaryOperator::Range:
            return "Range";
        case ast::BinaryOperator::Add:
            return "Add";
        case ast::BinaryOperator::Subtract:
            return "Subtract";
        case ast::BinaryOperator::Multiply:
            return "Multiply";
        case ast::BinaryOperator::Divide:
            return "Divide";
        case ast::BinaryOperator::Modulo:
            return "Modulo";
    }

    return "Unknown";
}

void printExpr(std::ostream& out, const SourceManager& sources, const ast::Expr& expr, int depth);
void printStmt(std::ostream& out, const SourceManager& sources, const ast::Stmt& stmt, int depth);
void printTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::TypeSyntax& type, int depth);

void printNamedTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::NamedTypeSyntax& type,
                           int depth) {
    writeIndent(out, depth);
    out << "NamedTypeSyntax name=" << wrapInQuotes(sources.text(type.name().span)) << '\n';
}

// No `default:` case, same idiom as the other kind-name helpers.
std::string_view referenceMutabilityName(ast::ReferenceMutability mutability) {
    switch (mutability) {
        case ast::ReferenceMutability::Immutable:
            return "Immutable";
        case ast::ReferenceMutability::Mutable:
            return "Mutable";
    }

    return "Unknown";
}

void printReferenceTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::ReferenceTypeSyntax& type,
                               int depth) {
    writeIndent(out, depth);
    out << "ReferenceTypeSyntax mutability=" << referenceMutabilityName(type.mutability()) << '\n';

    writeIndent(out, depth + 1);
    out << "Referent\n";
    printTypeSyntax(out, sources, type.referent(), depth + 2);
}

void printSliceTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::SliceTypeSyntax& type,
                           int depth) {
    writeIndent(out, depth);
    out << "SliceTypeSyntax\n";

    writeIndent(out, depth + 1);
    out << "Element\n";
    printTypeSyntax(out, sources, type.element(), depth + 2);
}

void printArrayTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::ArrayTypeSyntax& type,
                           int depth) {
    writeIndent(out, depth);
    out << "ArrayTypeSyntax\n";

    writeIndent(out, depth + 1);
    out << "Element\n";
    printTypeSyntax(out, sources, type.element(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Length\n";
    printExpr(out, sources, type.length(), depth + 2);
}

void printUnitTypeSyntax(std::ostream& out, const SourceManager&, const ast::UnitTypeSyntax&, int depth) {
    writeIndent(out, depth);
    out << "UnitTypeSyntax\n";
}

void printGenericTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::GenericTypeSyntax& type,
                             int depth) {
    writeIndent(out, depth);
    out << "GenericTypeSyntax name=" << wrapInQuotes(sources.text(type.name().span)) << '\n';

    for (const auto& argument : type.arguments()) {
        writeIndent(out, depth + 1);
        out << "Argument\n";
        printTypeSyntax(out, sources, *argument, depth + 2);
    }
}

// TypeSyntaxKind is fully implemented today: no `default:` case, so
// -Wswitch fires the moment a 7th TypeSyntaxKind is added without a case
// here.
void printTypeSyntax(std::ostream& out, const SourceManager& sources, const ast::TypeSyntax& type, int depth) {
    switch (type.kind()) {
        case ast::TypeSyntaxKind::Named:
            printNamedTypeSyntax(out, sources, static_cast<const ast::NamedTypeSyntax&>(type), depth);
            return;
        case ast::TypeSyntaxKind::Reference:
            printReferenceTypeSyntax(out, sources, static_cast<const ast::ReferenceTypeSyntax&>(type), depth);
            return;
        case ast::TypeSyntaxKind::Array:
            printArrayTypeSyntax(out, sources, static_cast<const ast::ArrayTypeSyntax&>(type), depth);
            return;
        case ast::TypeSyntaxKind::Slice:
            printSliceTypeSyntax(out, sources, static_cast<const ast::SliceTypeSyntax&>(type), depth);
            return;
        case ast::TypeSyntaxKind::Unit:
            printUnitTypeSyntax(out, sources, static_cast<const ast::UnitTypeSyntax&>(type), depth);
            return;
        case ast::TypeSyntaxKind::Generic:
            printGenericTypeSyntax(out, sources, static_cast<const ast::GenericTypeSyntax&>(type), depth);
            return;
    }
}

void printParam(std::ostream& out, const SourceManager& sources, const ast::Param& param, int depth) {
    writeIndent(out, depth);
    out << "Param name=" << wrapInQuotes(sources.text(param.name.span)) << '\n';
    printTypeSyntax(out, sources, *param.type, depth + 1);
}

void printLiteralExpr(std::ostream& out, const SourceManager& sources, const ast::LiteralExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "LiteralExpr kind=" << literalKindName(expr.literalKind())
        << " lexeme=" << wrapInQuotes(escapeLexeme(sources.text(expr.span()))) << '\n';
}

void printIdentifierExpr(std::ostream& out, const SourceManager& sources, const ast::IdentifierExpr& expr,
                          int depth) {
    writeIndent(out, depth);
    out << "IdentifierExpr name=" << wrapInQuotes(sources.text(expr.name().span)) << '\n';
}

void printCallExpr(std::ostream& out, const SourceManager& sources, const ast::CallExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "CallExpr\n";
    printExpr(out, sources, expr.callee(), depth + 1);
    for (const auto& argument : expr.arguments()) {
        printExpr(out, sources, *argument, depth + 1);
    }
}

void printParenExpr(std::ostream& out, const SourceManager& sources, const ast::ParenExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "ParenExpr\n";
    printExpr(out, sources, expr.inner(), depth + 1);
}

void printUnaryExpr(std::ostream& out, const SourceManager& sources, const ast::UnaryExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "UnaryExpr op=" << unaryOperatorName(expr.op()) << '\n';

    writeIndent(out, depth + 1);
    out << "Operand\n";
    printExpr(out, sources, expr.operand(), depth + 2);
}

void printBinaryExpr(std::ostream& out, const SourceManager& sources, const ast::BinaryExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "BinaryExpr op=" << binaryOperatorName(expr.op()) << '\n';

    writeIndent(out, depth + 1);
    out << "Left\n";
    printExpr(out, sources, expr.left(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Right\n";
    printExpr(out, sources, expr.right(), depth + 2);
}

void printAssignmentExpr(std::ostream& out, const SourceManager& sources, const ast::AssignmentExpr& expr,
                          int depth) {
    writeIndent(out, depth);
    out << "AssignmentExpr\n";

    writeIndent(out, depth + 1);
    out << "Target\n";
    printExpr(out, sources, expr.target(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Value\n";
    printExpr(out, sources, expr.value(), depth + 2);
}

void printArrayLiteralExpr(std::ostream& out, const SourceManager& sources, const ast::ArrayLiteralExpr& expr,
                            int depth) {
    writeIndent(out, depth);
    out << "ArrayLiteralExpr\n";

    for (const auto& element : expr.elements()) {
        writeIndent(out, depth + 1);
        out << "Element\n";
        printExpr(out, sources, *element, depth + 2);
    }
}

void printIndexExpr(std::ostream& out, const SourceManager& sources, const ast::IndexExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "IndexExpr\n";

    writeIndent(out, depth + 1);
    out << "Object\n";
    printExpr(out, sources, expr.object(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Index\n";
    printExpr(out, sources, expr.index(), depth + 2);
}

void printMemberExpr(std::ostream& out, const SourceManager& sources, const ast::MemberExpr& expr, int depth) {
    writeIndent(out, depth);
    out << "MemberExpr member=" << wrapInQuotes(sources.text(expr.member().span)) << '\n';

    writeIndent(out, depth + 1);
    out << "Object\n";
    printExpr(out, sources, expr.object(), depth + 2);
}

void printUnitExpr(std::ostream& out, const SourceManager&, const ast::UnitExpr&, int depth) {
    writeIndent(out, depth);
    out << "UnitExpr\n";
}

// operatorSpan is never printed - AstPrinter shows syntactic shape, not
// source offsets, matching AssignmentExpr's operatorSpan omission.
void printErrorPropagationExpr(std::ostream& out, const SourceManager& sources, const ast::ErrorPropagationExpr& expr,
                                int depth) {
    writeIndent(out, depth);
    out << "ErrorPropagationExpr\n";

    writeIndent(out, depth + 1);
    out << "Operand\n";
    printExpr(out, sources, expr.operand(), depth + 2);
}

// No `default:` case: ExprKind is fully implemented today, so -Wswitch
// should catch a forgotten case the moment a new ExprKind is added.
void printExpr(std::ostream& out, const SourceManager& sources, const ast::Expr& expr, int depth) {
    switch (expr.kind()) {
        case ast::ExprKind::Literal:
            printLiteralExpr(out, sources, static_cast<const ast::LiteralExpr&>(expr), depth);
            return;
        case ast::ExprKind::Identifier:
            printIdentifierExpr(out, sources, static_cast<const ast::IdentifierExpr&>(expr), depth);
            return;
        case ast::ExprKind::Call:
            printCallExpr(out, sources, static_cast<const ast::CallExpr&>(expr), depth);
            return;
        case ast::ExprKind::Paren:
            printParenExpr(out, sources, static_cast<const ast::ParenExpr&>(expr), depth);
            return;
        case ast::ExprKind::Unary:
            printUnaryExpr(out, sources, static_cast<const ast::UnaryExpr&>(expr), depth);
            return;
        case ast::ExprKind::Binary:
            printBinaryExpr(out, sources, static_cast<const ast::BinaryExpr&>(expr), depth);
            return;
        case ast::ExprKind::Assignment:
            printAssignmentExpr(out, sources, static_cast<const ast::AssignmentExpr&>(expr), depth);
            return;
        case ast::ExprKind::ArrayLiteral:
            printArrayLiteralExpr(out, sources, static_cast<const ast::ArrayLiteralExpr&>(expr), depth);
            return;
        case ast::ExprKind::Index:
            printIndexExpr(out, sources, static_cast<const ast::IndexExpr&>(expr), depth);
            return;
        case ast::ExprKind::Member:
            printMemberExpr(out, sources, static_cast<const ast::MemberExpr&>(expr), depth);
            return;
        case ast::ExprKind::Unit:
            printUnitExpr(out, sources, static_cast<const ast::UnitExpr&>(expr), depth);
            return;
        case ast::ExprKind::ErrorPropagation:
            printErrorPropagationExpr(out, sources, static_cast<const ast::ErrorPropagationExpr&>(expr), depth);
            return;
    }
}

void printBlockStmt(std::ostream& out, const SourceManager& sources, const ast::BlockStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "BlockStmt\n";
    for (const auto& statement : stmt.statements()) {
        printStmt(out, sources, *statement, depth + 1);
    }
}

void printExprStmt(std::ostream& out, const SourceManager& sources, const ast::ExprStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "ExprStmt\n";
    printExpr(out, sources, stmt.expr(), depth + 1);
}

void printVarDeclStmt(std::ostream& out, const SourceManager& sources, const ast::VarDeclStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "VarDeclStmt binding=" << bindingKindName(stmt.binding())
        << " name=" << wrapInQuotes(sources.text(stmt.name().span)) << '\n';

    if (const ast::TypeSyntax* type = stmt.type(); type != nullptr) {
        writeIndent(out, depth + 1);
        out << "Type\n";
        printTypeSyntax(out, sources, *type, depth + 2);
    }

    writeIndent(out, depth + 1);
    out << "Initializer\n";
    printExpr(out, sources, stmt.initializer(), depth + 2);
}

void printReturnStmt(std::ostream& out, const SourceManager& sources, const ast::ReturnStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "ReturnStmt\n";

    if (const ast::Expr* value = stmt.value(); value != nullptr) {
        writeIndent(out, depth + 1);
        out << "Value\n";
        printExpr(out, sources, *value, depth + 2);
    }
}

// `kind=If`/`kind=ElseIf` is debugging output only, derived purely from
// branch position (index 0 vs. later) - the AST itself has no
// BranchKind, since vector order already fully disambiguates this the
// same way CallExpr/BinaryExpr/AssignmentExpr already rely on child
// position rather than a labeling field.
void printIfBranch(std::ostream& out, const SourceManager& sources, const ast::IfBranch& branch, bool isInitial,
                    int depth) {
    writeIndent(out, depth);
    out << "Branch kind=" << (isInitial ? "If" : "ElseIf") << '\n';

    writeIndent(out, depth + 1);
    out << "Condition\n";
    printExpr(out, sources, *branch.condition, depth + 2);

    writeIndent(out, depth + 1);
    out << "Body\n";
    printBlockStmt(out, sources, *branch.body, depth + 2);
}

void printIfStmt(std::ostream& out, const SourceManager& sources, const ast::IfStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "IfStmt\n";

    bool isInitial = true;
    for (const ast::IfBranch& branch : stmt.branches()) {
        printIfBranch(out, sources, branch, isInitial, depth + 1);
        isInitial = false;
    }

    if (const std::optional<ast::ElseClause>& elseClause = stmt.elseClause(); elseClause.has_value()) {
        writeIndent(out, depth + 1);
        out << "Else\n";
        printBlockStmt(out, sources, *elseClause->body, depth + 2);
    }
}

void printWhileStmt(std::ostream& out, const SourceManager& sources, const ast::WhileStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "WhileStmt\n";

    writeIndent(out, depth + 1);
    out << "Condition\n";
    printExpr(out, sources, stmt.condition(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Body\n";
    printBlockStmt(out, sources, stmt.body(), depth + 2);
}

void printForStmt(std::ostream& out, const SourceManager& sources, const ast::ForStmt& stmt, int depth) {
    writeIndent(out, depth);
    out << "ForStmt variable=" << wrapInQuotes(sources.text(stmt.variable().span)) << '\n';

    writeIndent(out, depth + 1);
    out << "Iterable\n";
    printExpr(out, sources, stmt.iterable(), depth + 2);

    writeIndent(out, depth + 1);
    out << "Body\n";
    printBlockStmt(out, sources, stmt.body(), depth + 2);
}

// No `default:` case: StmtKind is fully implemented today.
void printStmt(std::ostream& out, const SourceManager& sources, const ast::Stmt& stmt, int depth) {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            printBlockStmt(out, sources, static_cast<const ast::BlockStmt&>(stmt), depth);
            return;
        case ast::StmtKind::Expr:
            printExprStmt(out, sources, static_cast<const ast::ExprStmt&>(stmt), depth);
            return;
        case ast::StmtKind::VarDecl:
            printVarDeclStmt(out, sources, static_cast<const ast::VarDeclStmt&>(stmt), depth);
            return;
        case ast::StmtKind::Return:
            printReturnStmt(out, sources, static_cast<const ast::ReturnStmt&>(stmt), depth);
            return;
        case ast::StmtKind::If:
            printIfStmt(out, sources, static_cast<const ast::IfStmt&>(stmt), depth);
            return;
        case ast::StmtKind::While:
            printWhileStmt(out, sources, static_cast<const ast::WhileStmt&>(stmt), depth);
            return;
        case ast::StmtKind::For:
            printForStmt(out, sources, static_cast<const ast::ForStmt&>(stmt), depth);
            return;
    }
}

void printFunctionDecl(std::ostream& out, const SourceManager& sources, const ast::FunctionDecl& decl, int depth) {
    writeIndent(out, depth);
    out << "FunctionDecl name=" << wrapInQuotes(sources.text(decl.name().span))
        << " public=" << (decl.isPublic() ? "true" : "false") << '\n';

    if (!decl.params().empty()) {
        writeIndent(out, depth + 1);
        out << "Parameters\n";
        for (const auto& param : decl.params()) {
            printParam(out, sources, param, depth + 2);
        }
    }

    if (const ast::TypeSyntax* returnType = decl.returnType(); returnType != nullptr) {
        writeIndent(out, depth + 1);
        out << "ReturnType\n";
        printTypeSyntax(out, sources, *returnType, depth + 2);
    }

    printBlockStmt(out, sources, decl.body(), depth + 1);
}

// No `default:` case: DeclKind is fully implemented today.
void printDecl(std::ostream& out, const SourceManager& sources, const ast::Decl& decl, int depth) {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            printFunctionDecl(out, sources, static_cast<const ast::FunctionDecl&>(decl), depth);
            return;
    }
}

} // namespace

void printAst(std::ostream& out, const SourceManager& sources, const ast::SourceFile& file) {
    out << "SourceFile\n";
    for (const auto& decl : file.declarations()) {
        printDecl(out, sources, *decl, 1);
    }
}

// No `default:` case: ParseErrorKind is fully implemented today.
std::string formatParseError(const SourceManager& sources, const parser::ParseError& error) {
    const SourceManager::LineColumn where = sources.lineColumn(error.span.begin());

    std::ostringstream message;
    message << "kaicc: parse error at " << where.line << ':' << where.column << ": ";

    switch (error.kind) {
        case parser::ParseErrorKind::UnexpectedToken:
            if (error.expected.has_value()) {
                message << "expected " << tokenKindName(*error.expected) << ", got " << tokenKindName(error.actual);
            } else {
                message << "unexpected " << tokenKindName(error.actual);
            }
            break;
        case parser::ParseErrorKind::InvalidToken: {
            const std::string_view text = sources.text(error.span);
            if (!text.empty()) {
                message << "invalid token " << wrapInQuotes(escapeLexeme(text));
            } else {
                message << "invalid token";
            }
            break;
        }
        case parser::ParseErrorKind::UnsupportedSyntax:
            message << "unsupported syntax starting with " << tokenKindName(error.actual);
            break;
    }

    return message.str();
}

int runAstCommand(SourceManager& sources, const std::filesystem::path& path, std::ostream& out, std::ostream& err) {
    const auto loaded = sources.loadFile(path);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << path.string() << "': " << loaded.error().message() << '\n';
        return 2;
    }

    parser::Parser astParser(sources, *loaded);
    const auto parsed = astParser.parseSourceFile();
    if (!parsed) {
        err << formatParseError(sources, parsed.error()) << '\n';
        return 4;
    }

    printAst(out, sources, *parsed);
    return 0;
}

} // namespace kai::cli
