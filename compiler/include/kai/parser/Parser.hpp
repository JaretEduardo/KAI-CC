#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/lexer/Lexer.hpp"
#include "kai/lexer/Token.hpp"
#include "kai/lexer/TokenKind.hpp"
#include "kai/parser/ParseError.hpp"
#include "kai/source/SourceLocation.hpp"
#include "kai/source/SourceManager.hpp"

#include <expected>
#include <optional>
#include <vector>

namespace kai::parser {

/// The initial KAI recursive-descent parser.
///
/// Parses one file's token stream into the existing syntax AST
/// (kai::ast). Recognizes function declarations with a comma-separated,
/// named-type-only parameter list and an optional named return type;
/// `let`/`mut` variable declarations and `return` statements; and the
/// full expression precedence ladder (assignment, logical or/and,
/// equality, comparison, range, additive, multiplicative, unary, and
/// postfix/primary) built from literals, identifiers, calls, and
/// parenthesized expressions - enough for:
///
///     fn add(a: i32, b: i32) -> i32 {
///         return a + b
///     }
///
///     fn main() {
///         let x = 20
///         mut y = 22
///         y = y + x
///         print(y)
///     }
///
/// Parser recognizes syntax only: it performs no semantic analysis, no
/// type checking, and constructs no Diagnostic (that subsystem does not
/// exist yet). On malformed, unsupported, or lexically-invalid input it
/// stops at the first error and returns a ParseError describing exactly
/// where and why - there is no error recovery in this milestone.
///
/// Parser owns its own Lexer (constructed internally from the same
/// (SourceManager, FileId) pair passed to the constructor) and never
/// exposes it; this keeps Parser's lifetime contract identical to
/// Lexer's own (the referenced SourceManager must outlive it) without
/// introducing a second object callers need to keep alive in sync.
class Parser {
public:
    Parser(const SourceManager& sources, FileId file);

    /// Parses the entire file as a sequence of top-level declarations.
    ParseResult<ast::SourceFile> parseSourceFile();

private:
    // --- token navigation ---
    // One-token lookahead is sufficient for the currently-implemented
    // grammar subset (and for the full precedence-climbing shape
    // GRAMMAR.md's expression grammar already calls for). This is not
    // an architectural ceiling: bounded additional lookahead may be
    // introduced later if a real future grammar rule genuinely needs
    // it, without requiring a TokenStream abstraction.
    Token current() const noexcept;
    Token advance();
    bool check(TokenKind kind) const noexcept;
    bool match(TokenKind kind);
    ParseResult<Token> expect(TokenKind kind);

    /// Consumes zero or more Newline/Semicolon tokens. Never called
    /// from expression parsing - the Lexer already suppresses physical
    /// newlines inside (...)/[...], so nothing needs to be duplicated
    /// here, and Newline/Semicolon are only ever statement/declaration
    /// separators, never legal inside an expression.
    void skipSeparators();

    /// Builds a ParseError anchored at the current token and returns it
    /// wrapped as std::unexpected, ready to convert into any
    /// ParseResult<T>. This is the single place that enforces the
    /// central invalid-token policy: regardless of which `kind` the
    /// caller requests, if the current token is TokenKind::Invalid the
    /// reported kind is unconditionally overridden to
    /// ParseErrorKind::InvalidToken. Centralizing this here means no
    /// individual parse function has to remember to special-case
    /// Invalid itself.
    std::unexpected<ParseError> fail(ParseErrorKind kind, std::optional<TokenKind> expectedKind = std::nullopt) const;

    // --- declarations ---
    ParseResult<ast::DeclPtr> parseDeclaration();
    ParseResult<ast::DeclPtr> parseFunctionDecl(bool isPublic, SourceLocation begin);
    ParseResult<std::vector<ast::Param>> parseParamList();
    ParseResult<ast::TypeSyntaxPtr> parseTypeSyntax();

    // --- statements ---
    ParseResult<ast::BlockPtr> parseBlock();
    ParseResult<ast::StmtPtr> parseStatement();
    ParseResult<ast::StmtPtr> parseVarDeclStmt();
    ParseResult<ast::StmtPtr> parseReturnStmt();

    // --- expressions ---
    // Precedence ladder, lowest to highest (GRAMMAR.md §26-41), each tier
    // a plain recursive-descent function - no Pratt/precedence-table
    // machinery. parseExpression is the single entry point; every other
    // tier is reached only by the one above it.
    ParseResult<ast::ExprPtr> parseExpression();
    ParseResult<ast::ExprPtr> parseAssignment();
    ParseResult<ast::ExprPtr> parseLogicalOr();
    ParseResult<ast::ExprPtr> parseLogicalAnd();
    ParseResult<ast::ExprPtr> parseEquality();
    ParseResult<ast::ExprPtr> parseComparison();
    ParseResult<ast::ExprPtr> parseRange();
    ParseResult<ast::ExprPtr> parseAdditive();
    ParseResult<ast::ExprPtr> parseMultiplicative();
    ParseResult<ast::ExprPtr> parseUnary();
    ParseResult<ast::ExprPtr> parsePostfixExpression();
    ParseResult<ast::ExprPtr> parsePrimary();
    ParseResult<ast::ExprPtr> parseParenExpression();
    ParseResult<ast::ExprPtr> parseCallSuffix(ast::ExprPtr callee);
    ParseResult<std::vector<ast::ExprPtr>> parseArgumentList();

    FileId file_;
    Lexer lexer_;
    Token current_;
};

} // namespace kai::parser
