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

void Parser::skipNewlines() {
    while (check(TokenKind::Newline)) {
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
    if (check(TokenKind::Amp)) {
        return parseReferenceTypeSyntax();
    }
    if (check(TokenKind::LeftBracket)) {
        return parseBracketTypeSyntax();
    }
    if (check(TokenKind::LeftParen)) {
        // Unit type () (GRAMMAR.md §18) - exactly "(" ")", never a
        // parenthesized type: (i32) is not valid type syntax, so no
        // inner parseTypeSyntax() call happens here.
        const Token openParen = advance();
        auto closeParen = expect(TokenKind::RightParen);
        if (!closeParen) {
            return std::unexpected(closeParen.error());
        }
        const SourceSpan span(openParen.span().begin(), closeParen->span().end());
        ast::TypeSyntaxPtr type = std::make_unique<ast::UnitTypeSyntax>(span);
        return type;
    }

    auto nameTok = expect(TokenKind::Identifier);
    if (!nameTok) {
        return std::unexpected(nameTok.error());
    }

    if (check(TokenKind::Less)) {
        return parseGenericTypeSyntax(*nameTok);
    }

    ast::TypeSyntaxPtr type = std::make_unique<ast::NamedTypeSyntax>(ast::Identifier{nameTok->span()}, nameTok->span());
    return type;
}

ParseResult<ast::TypeSyntaxPtr> Parser::parseGenericTypeSyntax(Token nameToken) {
    auto lessTok = expect(TokenKind::Less);
    if (!lessTok) {
        return std::unexpected(lessTok.error());
    }

    // Newlines are tolerated inside an already-recognized generic
    // argument list (GRAMMAR.md §17), never Semicolon - skipNewlines()
    // is used here instead of skipSeparators() specifically for that
    // reason.
    skipNewlines();

    std::vector<ast::TypeSyntaxPtr> arguments; // type_list requires >= 1 - the loop below always runs once.

    while (true) {
        auto argument = parseTypeSyntax(); // recursion: Result<&str, E>, Option<[i32]>, Result<Option<i32>, E>, ...
        if (!argument) {
            return std::unexpected(argument.error());
        }
        arguments.push_back(std::move(*argument));

        // Tolerate a newline before deciding comma vs. closing `>` -
        // this is also "before the final `>`" whenever no comma follows.
        skipNewlines();

        if (!match(TokenKind::Comma)) {
            break;
        }

        skipNewlines(); // tolerate a newline right after a comma
    }

    auto greaterTok = expect(TokenKind::Greater);
    if (!greaterTok) {
        return std::unexpected(greaterTok.error());
    }

    const SourceSpan span(nameToken.span().begin(), greaterTok->span().end());
    ast::TypeSyntaxPtr type =
        std::make_unique<ast::GenericTypeSyntax>(ast::Identifier{nameToken.span()}, std::move(arguments), span);
    return type;
}

ParseResult<ast::TypeSyntaxPtr> Parser::parseReferenceTypeSyntax() {
    auto ampTok = expect(TokenKind::Amp);
    if (!ampTok) {
        return std::unexpected(ampTok.error());
    }

    ast::ReferenceMutability mutability = ast::ReferenceMutability::Immutable;
    SourceSpan operatorSpan = ampTok->span();

    // `&mut T` and `& mut T` both tokenize as Amp then KwMut - whitespace
    // between them is not significant, exactly like the value-level `&`/
    // `&mut` unary operator.
    if (check(TokenKind::KwMut)) {
        const Token mutTok = advance();
        mutability = ast::ReferenceMutability::Mutable;
        operatorSpan = SourceSpan(ampTok->span().begin(), mutTok.span().end());
    }

    auto referent = parseTypeSyntax(); // recursion: &[T], &mut [T], &&T, ...
    if (!referent) {
        return std::unexpected(referent.error());
    }

    const SourceSpan span(operatorSpan.begin(), (*referent)->span().end());
    ast::TypeSyntaxPtr type =
        std::make_unique<ast::ReferenceTypeSyntax>(mutability, operatorSpan, std::move(*referent), span);
    return type;
}

ParseResult<ast::TypeSyntaxPtr> Parser::parseBracketTypeSyntax() {
    auto openBracket = expect(TokenKind::LeftBracket);
    if (!openBracket) {
        return std::unexpected(openBracket.error());
    }

    auto element = parseTypeSyntax(); // recursion: [[i32; 4]], [&str], ...
    if (!element) {
        return std::unexpected(element.error());
    }

    if (check(TokenKind::Semicolon)) {
        advance();

        // GRAMMAR.md §15 currently requires integer_literal specifically,
        // not a general expression - constant-expression lengths are not
        // supported yet.
        auto lengthTok = expect(TokenKind::IntegerLiteral);
        if (!lengthTok) {
            return std::unexpected(lengthTok.error());
        }

        auto closeBracket = expect(TokenKind::RightBracket);
        if (!closeBracket) {
            return std::unexpected(closeBracket.error());
        }

        ast::ExprPtr length = std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Integer, lengthTok->span());
        const SourceSpan span(openBracket->span().begin(), closeBracket->span().end());
        ast::TypeSyntaxPtr type = std::make_unique<ast::ArrayTypeSyntax>(std::move(*element), std::move(length), span);
        return type;
    }

    auto closeBracket = expect(TokenKind::RightBracket);
    if (!closeBracket) {
        return std::unexpected(closeBracket.error());
    }

    const SourceSpan span(openBracket->span().begin(), closeBracket->span().end());
    ast::TypeSyntaxPtr type = std::make_unique<ast::SliceTypeSyntax>(std::move(*element), span);
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
            return parseIfStmt();

        case TokenKind::KwWhile:
            return parseWhileStmt();

        case TokenKind::KwFor:
            return parseForStmt();

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

ParseResult<ast::StmtPtr> Parser::parseIfStmt() {
    std::vector<ast::IfBranch> branches;
    std::optional<ast::ElseClause> elseClause;

    SourceLocation branchBegin = current().span().begin(); // `if`
    auto ifTok = expect(TokenKind::KwIf);
    if (!ifTok) {
        return std::unexpected(ifTok.error());
    }

    while (true) {
        auto condition = parseExpression();
        if (!condition) {
            return std::unexpected(condition.error());
        }

        auto body = parseBlock();
        if (!body) {
            return std::unexpected(body.error());
        }

        const SourceSpan branchSpan(branchBegin, (*body)->span().end());
        branches.push_back(ast::IfBranch{std::move(*condition), std::move(*body), branchSpan});

        // Narrow lookahead: skip Newline only (never Semicolon) to see
        // whether the same if_statement continues with `else`
        // (GRAMMAR.md §23's documented newline-before-else rule). If no
        // `else` follows, any Newlines consumed here would have been
        // consumed by the enclosing parseBlock()'s own skipSeparators()
        // anyway - harmless.
        while (check(TokenKind::Newline)) {
            advance();
        }

        if (!check(TokenKind::KwElse)) {
            break;
        }
        const Token elseTok = advance(); // consume `else`

        // No newline tolerance between `else` and what follows it: not
        // `else\nif`, not `else\n{`. Only the immediately-adjacent forms
        // are valid, so no skipSeparators() call here.
        if (match(TokenKind::KwIf)) {
            branchBegin = elseTok.span().begin();
            continue;
        }

        auto finalBody = parseBlock();
        if (!finalBody) {
            return std::unexpected(finalBody.error());
        }

        const SourceSpan elseSpan(elseTok.span().begin(), (*finalBody)->span().end());
        elseClause = ast::ElseClause{std::move(*finalBody), elseSpan};
        break;
    }

    const SourceSpan span(branches.front().span.begin(),
                           elseClause ? elseClause->span.end() : branches.back().span.end());
    ast::StmtPtr stmt = std::make_unique<ast::IfStmt>(std::move(branches), std::move(elseClause), span);
    return stmt;
}

ParseResult<ast::StmtPtr> Parser::parseWhileStmt() {
    auto whileTok = expect(TokenKind::KwWhile);
    if (!whileTok) {
        return std::unexpected(whileTok.error());
    }

    auto condition = parseExpression();
    if (!condition) {
        return std::unexpected(condition.error());
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }

    const SourceSpan span(whileTok->span().begin(), (*body)->span().end());
    ast::StmtPtr stmt = std::make_unique<ast::WhileStmt>(std::move(*condition), std::move(*body), span);
    return stmt;
}

ParseResult<ast::StmtPtr> Parser::parseForStmt() {
    auto forTok = expect(TokenKind::KwFor);
    if (!forTok) {
        return std::unexpected(forTok.error());
    }

    auto varTok = expect(TokenKind::Identifier);
    if (!varTok) {
        return std::unexpected(varTok.error());
    }

    if (auto inTok = expect(TokenKind::KwIn); !inTok) {
        return std::unexpected(inTok.error());
    }

    // The iterable is an arbitrary expression - the Parser never assumes
    // it is a range (GRAMMAR.md §25). `{` is not a continuation token
    // for any expression precedence tier, so parseExpression() naturally
    // stops with current() == LeftBrace and parseBlock() picks up from
    // there; no delimiter hack is needed.
    auto iterable = parseExpression();
    if (!iterable) {
        return std::unexpected(iterable.error());
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }

    const SourceSpan span(forTok->span().begin(), (*body)->span().end());
    ast::StmtPtr stmt = std::make_unique<ast::ForStmt>(ast::Identifier{varTok->span()}, std::move(*iterable),
                                                        std::move(*body), span);
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

        if (check(TokenKind::LeftBracket)) {
            auto indexed = parseIndexSuffix(std::move(*expr));
            if (!indexed) {
                return indexed;
            }
            expr = std::move(indexed);
            continue;
        }

        if (check(TokenKind::Dot)) {
            auto member = parseMemberSuffix(std::move(*expr));
            if (!member) {
                return member;
            }
            expr = std::move(member);
            continue;
        }

        if (check(TokenKind::Question)) {
            const Token questionTok = advance();
            const SourceSpan span((*expr)->span().begin(), questionTok.span().end());
            ast::ExprPtr propagated =
                std::make_unique<ast::ErrorPropagationExpr>(std::move(*expr), questionTok.span(), span);
            expr = std::move(propagated);
            continue;
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
            return parseArrayLiteral();

        default:
            return fail(ParseErrorKind::UnexpectedToken);
    }
}

ParseResult<ast::ExprPtr> Parser::parseParenExpression() {
    auto openParen = expect(TokenKind::LeftParen);
    if (!openParen) {
        return std::unexpected(openParen.error());
    }

    // () as an expression is the unit value (GRAMMAR.md §41,
    // unit_expression), checked before attempting parseExpression() -
    // the same "check for the empty-form closer first" idiom already
    // used by parseArrayLiteral()/parseArgumentList() for []/f().
    if (check(TokenKind::RightParen)) {
        const Token closeParen = advance();
        const SourceSpan span(openParen->span().begin(), closeParen.span().end());
        ast::ExprPtr expr = std::make_unique<ast::UnitExpr>(span);
        return expr;
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

ParseResult<ast::ExprPtr> Parser::parseArrayLiteral() {
    auto openBracket = expect(TokenKind::LeftBracket);
    if (!openBracket) {
        return std::unexpected(openBracket.error());
    }

    std::vector<ast::ExprPtr> elements;

    // Structurally identical to parseArgumentList() (a trailing comma is
    // rejected there too): GRAMMAR.md §42's array_literal production has
    // the same "expression { , expression }" shape as §37's argument_list.
    if (!check(TokenKind::RightBracket)) {
        while (true) {
            auto element = parseExpression();
            if (!element) {
                return std::unexpected(element.error());
            }
            elements.push_back(std::move(*element));

            if (!match(TokenKind::Comma)) {
                break;
            }
        }
    }

    auto closeBracket = expect(TokenKind::RightBracket);
    if (!closeBracket) {
        return std::unexpected(closeBracket.error());
    }

    const SourceSpan span(openBracket->span().begin(), closeBracket->span().end());
    ast::ExprPtr expr = std::make_unique<ast::ArrayLiteralExpr>(std::move(elements), span);
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

ParseResult<ast::ExprPtr> Parser::parseIndexSuffix(ast::ExprPtr object) {
    auto openBracket = expect(TokenKind::LeftBracket);
    if (!openBracket) {
        return std::unexpected(openBracket.error());
    }

    // The index is an arbitrary expression syntactically - no
    // indexability/integer-type/bounds checking belongs here.
    auto index = parseExpression();
    if (!index) {
        return std::unexpected(index.error());
    }

    auto closeBracket = expect(TokenKind::RightBracket);
    if (!closeBracket) {
        return std::unexpected(closeBracket.error());
    }

    const SourceSpan span(object->span().begin(), closeBracket->span().end());
    ast::ExprPtr expr = std::make_unique<ast::IndexExpr>(std::move(object), std::move(*index), span);
    return expr;
}

ParseResult<ast::ExprPtr> Parser::parseMemberSuffix(ast::ExprPtr object) {
    auto dot = expect(TokenKind::Dot);
    if (!dot) {
        return std::unexpected(dot.error());
    }

    auto memberTok = expect(TokenKind::Identifier);
    if (!memberTok) {
        return std::unexpected(memberTok.error());
    }

    const SourceSpan span(object->span().begin(), memberTok->span().end());
    ast::ExprPtr expr =
        std::make_unique<ast::MemberExpr>(std::move(object), ast::Identifier{memberTok->span()}, span);
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
