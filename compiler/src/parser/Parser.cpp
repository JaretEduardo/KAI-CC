#include "kai/parser/Parser.hpp"

#include <utility>

namespace kai::parser {

Parser::Parser(const SourceManager& sources, FileId file)
    : file_(file), lexer_(sources, file), current_(lexer_.nextToken()) {}

// --- token navigation ---

Token Parser::current() const noexcept { return current_; }

Token Parser::advance() {
    const Token consumed = current_;
    current_ = lexer_.nextToken();
    return consumed;
}

bool Parser::check(TokenKind kind) const noexcept { return current_.kind() == kind; }

bool Parser::match(TokenKind kind) {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

ParseResult<Token> Parser::expect(TokenKind kind) {
    if (check(kind)) {
        return advance();
    }
    return fail(ParseErrorKind::UnexpectedToken, kind);
}

void Parser::skipSeparators() {
    while (check(TokenKind::Newline) || check(TokenKind::Semicolon)) {
        advance();
    }
}

std::unexpected<ParseError> Parser::fail(ParseErrorKind kind, std::optional<TokenKind> expectedKind) const {
    if (current_.kind() == TokenKind::Invalid) {
        kind = ParseErrorKind::InvalidToken;
    }
    return std::unexpected(ParseError{kind, current_.span(), current_.kind(), expectedKind});
}

// --- declarations ---

ParseResult<ast::DeclPtr> Parser::parseDeclaration() {
    const SourceLocation begin = current().span().begin();
    const bool isPublic = match(TokenKind::KwPub);

    switch (current().kind()) {
        case TokenKind::KwFn:
            return parseFunctionDecl(isPublic, begin);

        case TokenKind::KwStruct:
        case TokenKind::KwEnum:
        case TokenKind::KwUse:
            // Valid GRAMMAR.md top-level declarations - no DeclKind for
            // them yet in this milestone's AST.
            return fail(ParseErrorKind::UnsupportedSyntax);

        default:
            return fail(ParseErrorKind::UnexpectedToken);
    }
}

ParseResult<ast::DeclPtr> Parser::parseFunctionDecl(bool isPublic, SourceLocation begin) {
    auto fnTok = expect(TokenKind::KwFn);
    if (!fnTok) {
        return std::unexpected(fnTok.error());
    }

    auto nameTok = expect(TokenKind::Identifier);
    if (!nameTok) {
        return std::unexpected(nameTok.error());
    }

    if (auto leftParen = expect(TokenKind::LeftParen); !leftParen) {
        return std::unexpected(leftParen.error());
    }

    auto params = parseParamList();
    if (!params) {
        return std::unexpected(params.error());
    }

    if (auto rightParen = expect(TokenKind::RightParen); !rightParen) {
        return std::unexpected(rightParen.error());
    }

    ast::TypeSyntaxPtr returnType;
    if (match(TokenKind::Arrow)) {
        auto parsedReturnType = parseTypeSyntax();
        if (!parsedReturnType) {
            return std::unexpected(parsedReturnType.error());
        }
        returnType = std::move(*parsedReturnType);
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }

    const SourceSpan span(begin, (*body)->span().end());
    ast::DeclPtr decl = std::make_unique<ast::FunctionDecl>(
        isPublic, ast::Identifier{nameTok->span()}, std::move(*params), std::move(returnType), std::move(*body), span);
    return decl;
}

ParseResult<std::vector<ast::Param>> Parser::parseParamList() {
    std::vector<ast::Param> params;

    if (check(TokenKind::RightParen)) {
        return params;
    }

    while (true) {
        auto nameTok = expect(TokenKind::Identifier);
        if (!nameTok) {
            return std::unexpected(nameTok.error());
        }

        if (auto colon = expect(TokenKind::Colon); !colon) {
            return std::unexpected(colon.error());
        }

        auto type = parseTypeSyntax();
        if (!type) {
            return std::unexpected(type.error());
        }

        const SourceSpan paramSpan(nameTok->span().begin(), (*type)->span().end());
        params.push_back(ast::Param{ast::Identifier{nameTok->span()}, std::move(*type), paramSpan});

        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    return params;
}

ParseResult<ast::TypeSyntaxPtr> Parser::parseTypeSyntax() {
    switch (current().kind()) {
        case TokenKind::Amp:
        case TokenKind::LeftBracket:
        case TokenKind::LeftParen:
            // &T / &mut T, [T] / [T; N], and () are valid GRAMMAR.md type
            // forms with no TypeSyntax node beyond Named yet.
            return fail(ParseErrorKind::UnsupportedSyntax);
        default:
            break;
    }

    auto nameTok = expect(TokenKind::Identifier);
    if (!nameTok) {
        return std::unexpected(nameTok.error());
    }

    if (check(TokenKind::Less)) {
        // Result<T, E>-shaped generic use-site syntax - valid grammar,
        // no GenericTypeSyntax node yet.
        return fail(ParseErrorKind::UnsupportedSyntax);
    }

    ast::TypeSyntaxPtr type = std::make_unique<ast::NamedTypeSyntax>(ast::Identifier{nameTok->span()}, nameTok->span());
    return type;
}

// --- statements ---

ParseResult<ast::BlockPtr> Parser::parseBlock() {
    auto openBrace = expect(TokenKind::LeftBrace);
    if (!openBrace) {
        return std::unexpected(openBrace.error());
    }

    skipSeparators();

    std::vector<ast::StmtPtr> statements;

    while (!check(TokenKind::RightBrace)) {
        if (check(TokenKind::EndOfFile)) {
            return fail(ParseErrorKind::UnexpectedToken, TokenKind::RightBrace);
        }

        auto stmt = parseStatement();
        if (!stmt) {
            return std::unexpected(stmt.error());
        }
        statements.push_back(std::move(*stmt));

        skipSeparators();
    }

    auto closeBrace = expect(TokenKind::RightBrace);
    if (!closeBrace) {
        return std::unexpected(closeBrace.error());
    }

    const SourceSpan span(openBrace->span().begin(), closeBrace->span().end());
    return std::make_unique<ast::BlockStmt>(std::move(statements), span);
}

ParseResult<ast::StmtPtr> Parser::parseStatement() {
    switch (current().kind()) {
        case TokenKind::KwLet:
        case TokenKind::KwMut:
            return parseVarDeclStmt();

        case TokenKind::KwReturn:
            return parseReturnStmt();

        case TokenKind::KwIf:
        case TokenKind::KwWhile:
        case TokenKind::KwFor:
            // Valid GRAMMAR.md statements - no StmtKind for them yet in
            // this milestone's AST.
            return fail(ParseErrorKind::UnsupportedSyntax);
        default:
            break;
    }

    auto expr = parseExpression();
    if (!expr) {
        return std::unexpected(expr.error());
    }

    if (!check(TokenKind::Newline) && !check(TokenKind::Semicolon) && !check(TokenKind::RightBrace) &&
        !check(TokenKind::EndOfFile)) {
        return fail(ParseErrorKind::UnexpectedToken);
    }

    const SourceSpan span = (*expr)->span();
    ast::StmtPtr stmt = std::make_unique<ast::ExprStmt>(std::move(*expr), span);
    return stmt;
}

ParseResult<ast::StmtPtr> Parser::parseVarDeclStmt() {
    const bool isMutable = check(TokenKind::KwMut);
    auto keywordTok = isMutable ? expect(TokenKind::KwMut) : expect(TokenKind::KwLet);
    if (!keywordTok) {
        return std::unexpected(keywordTok.error());
    }

    auto nameTok = expect(TokenKind::Identifier);
    if (!nameTok) {
        return std::unexpected(nameTok.error());
    }

    ast::TypeSyntaxPtr type;
    if (match(TokenKind::Colon)) {
        auto parsedType = parseTypeSyntax();
        if (!parsedType) {
            return std::unexpected(parsedType.error());
        }
        type = std::move(*parsedType);
    }

    if (auto eq = expect(TokenKind::Equal); !eq) {
        return std::unexpected(eq.error());
    }

    // The initializer is grammatically required (GRAMMAR.md §21). Its
    // value is never checked against the type annotation here - that is
    // semantic analysis's job, not the parser's.
    auto initializer = parseExpression();
    if (!initializer) {
        return std::unexpected(initializer.error());
    }

    if (!check(TokenKind::Newline) && !check(TokenKind::Semicolon) && !check(TokenKind::RightBrace) &&
        !check(TokenKind::EndOfFile)) {
        return fail(ParseErrorKind::UnexpectedToken);
    }

    const ast::BindingKind binding = isMutable ? ast::BindingKind::Mutable : ast::BindingKind::Immutable;
    const SourceSpan span(keywordTok->span().begin(), (*initializer)->span().end());
    ast::StmtPtr stmt = std::make_unique<ast::VarDeclStmt>(binding, ast::Identifier{nameTok->span()}, std::move(type),
                                                            std::move(*initializer), span);
    return stmt;
}

ParseResult<ast::StmtPtr> Parser::parseReturnStmt() {
    auto returnTok = expect(TokenKind::KwReturn);
    if (!returnTok) {
        return std::unexpected(returnTok.error());
    }

    ast::ExprPtr value;
    SourceLocation end = returnTok->span().end();

    // A terminator immediately after `return` means a bare return; this
    // lookahead happens before attempting to parse an expression at all,
    // so `return\nvalue` correctly becomes two statements rather than
    // `return value`.
    const bool atTerminator = check(TokenKind::Newline) || check(TokenKind::Semicolon) ||
                               check(TokenKind::RightBrace) || check(TokenKind::EndOfFile);
    if (!atTerminator) {
        auto expr = parseExpression();
        if (!expr) {
            return std::unexpected(expr.error());
        }
        value = std::move(*expr);
        end = value->span().end();
    }

    if (!check(TokenKind::Newline) && !check(TokenKind::Semicolon) && !check(TokenKind::RightBrace) &&
        !check(TokenKind::EndOfFile)) {
        return fail(ParseErrorKind::UnexpectedToken);
    }

    const SourceSpan span(returnTok->span().begin(), end);
    ast::StmtPtr stmt = std::make_unique<ast::ReturnStmt>(std::move(value), span);
    return stmt;
}

// --- expressions ---
//
// Precedence ladder, lowest to highest (GRAMMAR.md §26-41). Each tier is
// a plain recursive-descent function; left-associative tiers loop,
// assignment/unary recurse on themselves (right-associative/prefix), and
// range parses at most one ".." (GRAMMAR.md §32 uses "[...]", not
// "{...}").

ParseResult<ast::ExprPtr> Parser::parseExpression() { return parseAssignment(); }

ParseResult<ast::ExprPtr> Parser::parseAssignment() {
    auto left = parseLogicalOr();
    if (!left) {
        return left;
    }

    if (check(TokenKind::Equal)) {
        const Token eqTok = advance();
        auto value = parseAssignment(); // right-associative
        if (!value) {
            return value;
        }

        const SourceSpan span((*left)->span().begin(), (*value)->span().end());
        ast::ExprPtr expr =
            std::make_unique<ast::AssignmentExpr>(std::move(*left), eqTok.span(), std::move(*value), span);
        return expr;
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();
    if (!left) {
        return left;
    }

    while (check(TokenKind::PipePipe)) {
        const Token opTok = advance();
        auto right = parseLogicalAnd();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr = std::make_unique<ast::BinaryExpr>(ast::BinaryOperator::Or, opTok.span(),
                                                                std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseLogicalAnd() {
    auto left = parseEquality();
    if (!left) {
        return left;
    }

    while (check(TokenKind::AmpAmp)) {
        const Token opTok = advance();
        auto right = parseEquality();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr = std::make_unique<ast::BinaryExpr>(ast::BinaryOperator::And, opTok.span(),
                                                                std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseEquality() {
    auto left = parseComparison();
    if (!left) {
        return left;
    }

    while (check(TokenKind::EqualEqual) || check(TokenKind::BangEqual)) {
        const Token opTok = advance();
        const ast::BinaryOperator op =
            opTok.kind() == TokenKind::EqualEqual ? ast::BinaryOperator::Equal : ast::BinaryOperator::NotEqual;

        auto right = parseComparison();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr =
            std::make_unique<ast::BinaryExpr>(op, opTok.span(), std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseComparison() {
    auto left = parseRange();
    if (!left) {
        return left;
    }

    while (check(TokenKind::Less) || check(TokenKind::LessEqual) || check(TokenKind::Greater) ||
           check(TokenKind::GreaterEqual)) {
        const Token opTok = advance();
        ast::BinaryOperator op = ast::BinaryOperator::Less;
        if (opTok.kind() == TokenKind::LessEqual) {
            op = ast::BinaryOperator::LessEqual;
        } else if (opTok.kind() == TokenKind::Greater) {
            op = ast::BinaryOperator::Greater;
        } else if (opTok.kind() == TokenKind::GreaterEqual) {
            op = ast::BinaryOperator::GreaterEqual;
        }

        auto right = parseRange();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr =
            std::make_unique<ast::BinaryExpr>(op, opTok.span(), std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseRange() {
    auto left = parseAdditive();
    if (!left) {
        return left;
    }

    if (check(TokenKind::DotDot)) {
        const Token opTok = advance();
        auto right = parseAdditive();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr = std::make_unique<ast::BinaryExpr>(ast::BinaryOperator::Range, opTok.span(),
                                                                std::move(*left), std::move(*right), span);
        return expr;
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseAdditive() {
    auto left = parseMultiplicative();
    if (!left) {
        return left;
    }

    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        const Token opTok = advance();
        const ast::BinaryOperator op =
            opTok.kind() == TokenKind::Plus ? ast::BinaryOperator::Add : ast::BinaryOperator::Subtract;

        auto right = parseMultiplicative();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr =
            std::make_unique<ast::BinaryExpr>(op, opTok.span(), std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseMultiplicative() {
    auto left = parseUnary();
    if (!left) {
        return left;
    }

    while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
        const Token opTok = advance();
        ast::BinaryOperator op = ast::BinaryOperator::Multiply;
        if (opTok.kind() == TokenKind::Slash) {
            op = ast::BinaryOperator::Divide;
        } else if (opTok.kind() == TokenKind::Percent) {
            op = ast::BinaryOperator::Modulo;
        }

        auto right = parseUnary();
        if (!right) {
            return right;
        }

        const SourceSpan span((*left)->span().begin(), (*right)->span().end());
        ast::ExprPtr expr =
            std::make_unique<ast::BinaryExpr>(op, opTok.span(), std::move(*left), std::move(*right), span);
        left = std::move(expr);
    }

    return left;
}

ParseResult<ast::ExprPtr> Parser::parseUnary() {
    if (!check(TokenKind::Bang) && !check(TokenKind::Minus) && !check(TokenKind::Amp)) {
        return parsePostfixExpression();
    }

    const Token opTok = advance();
    ast::UnaryOperator op = ast::UnaryOperator::Not;
    SourceSpan operatorSpan = opTok.span();

    if (opTok.kind() == TokenKind::Bang) {
        op = ast::UnaryOperator::Not;
    } else if (opTok.kind() == TokenKind::Minus) {
        op = ast::UnaryOperator::Negate;
    } else {
        // TokenKind::Amp. `&mut` and `& mut` both tokenize as Amp then
        // KwMut - whitespace between them is not significant - so both
        // spellings are accepted here identically. `&&x` lexes as a
        // single AmpAmp token (logical-and), never as two Amp tokens, so
        // it never reaches this branch as `& &x` would.
        if (check(TokenKind::KwMut)) {
            const Token mutTok = advance();
            op = ast::UnaryOperator::RefMut;
            operatorSpan = SourceSpan(opTok.span().begin(), mutTok.span().end());
        } else {
            op = ast::UnaryOperator::Ref;
        }
    }

    auto operand = parseUnary(); // right-associative prefix
    if (!operand) {
        return operand;
    }

    const SourceSpan span(operatorSpan.begin(), (*operand)->span().end());
    ast::ExprPtr expr = std::make_unique<ast::UnaryExpr>(op, operatorSpan, std::move(*operand), span);
    return expr;
}

ParseResult<ast::ExprPtr> Parser::parsePostfixExpression() {
    auto expr = parsePrimary();
    if (!expr) {
        return expr;
    }

    while (true) {
        if (check(TokenKind::LeftParen)) {
            auto call = parseCallSuffix(std::move(*expr));
            if (!call) {
                return call;
            }
            expr = std::move(call);
            continue;
        }

        if (check(TokenKind::Dot) || check(TokenKind::LeftBracket) || check(TokenKind::Question)) {
            // Valid GRAMMAR.md postfix forms (member access, indexing,
            // error propagation) - no corresponding Expr node yet.
            return fail(ParseErrorKind::UnsupportedSyntax);
        }

        break;
    }

    return expr;
}

ParseResult<ast::ExprPtr> Parser::parsePrimary() {
    switch (current().kind()) {
        case TokenKind::Identifier: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::IdentifierExpr>(ast::Identifier{tok.span()}, tok.span());
            return expr;
        }
        case TokenKind::IntegerLiteral: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Integer, tok.span());
            return expr;
        }
        case TokenKind::FloatLiteral: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Float, tok.span());
            return expr;
        }
        case TokenKind::StringLiteral: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::String, tok.span());
            return expr;
        }
        case TokenKind::CharLiteral: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Char, tok.span());
            return expr;
        }
        case TokenKind::KwTrue:
        case TokenKind::KwFalse: {
            const Token tok = advance();
            ast::ExprPtr expr = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Bool, tok.span());
            return expr;
        }
        case TokenKind::LeftParen:
            return parseParenExpression();

        case TokenKind::LeftBracket:
            // `[1, 2, 3]` array-literal syntax (GRAMMAR.md §41/§42) is
            // valid grammar with no ArrayLiteralExpr node yet.
            return fail(ParseErrorKind::UnsupportedSyntax);

        default:
            return fail(ParseErrorKind::UnexpectedToken);
    }
}

ParseResult<ast::ExprPtr> Parser::parseParenExpression() {
    auto openParen = expect(TokenKind::LeftParen);
    if (!openParen) {
        return std::unexpected(openParen.error());
    }

    auto inner = parseExpression();
    if (!inner) {
        return inner;
    }

    auto closeParen = expect(TokenKind::RightParen);
    if (!closeParen) {
        return std::unexpected(closeParen.error());
    }

    const SourceSpan span(openParen->span().begin(), closeParen->span().end());
    ast::ExprPtr expr = std::make_unique<ast::ParenExpr>(std::move(*inner), span);
    return expr;
}

ParseResult<ast::ExprPtr> Parser::parseCallSuffix(ast::ExprPtr callee) {
    auto openParen = expect(TokenKind::LeftParen);
    if (!openParen) {
        return std::unexpected(openParen.error());
    }

    auto arguments = parseArgumentList();
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    auto closeParen = expect(TokenKind::RightParen);
    if (!closeParen) {
        return std::unexpected(closeParen.error());
    }

    const SourceSpan span(callee->span().begin(), closeParen->span().end());
    ast::ExprPtr expr = std::make_unique<ast::CallExpr>(std::move(callee), std::move(*arguments), span);
    return expr;
}

ParseResult<std::vector<ast::ExprPtr>> Parser::parseArgumentList() {
    std::vector<ast::ExprPtr> arguments;

    if (check(TokenKind::RightParen)) {
        return arguments;
    }

    while (true) {
        auto arg = parseExpression();
        if (!arg) {
            return std::unexpected(arg.error());
        }
        arguments.push_back(std::move(*arg));

        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    return arguments;
}

// --- root ---

ParseResult<ast::SourceFile> Parser::parseSourceFile() {
    skipSeparators();

    std::vector<ast::DeclPtr> declarations;

    while (!check(TokenKind::EndOfFile)) {
        auto decl = parseDeclaration();
        if (!decl) {
            return std::unexpected(decl.error());
        }
        declarations.push_back(std::move(*decl));

        skipSeparators();
    }

    SourceSpan span;
    if (declarations.empty()) {
        span = SourceSpan::point(current().span().begin());
    } else {
        span = SourceSpan(declarations.front()->span().begin(), declarations.back()->span().end());
    }

    return ast::SourceFile(file_, std::move(declarations), span);
}

} // namespace kai::parser
