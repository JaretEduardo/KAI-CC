#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/ast/TypeSyntax.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace kai::ast {

/// This vocabulary covers every Stmt node kind implemented so far.
enum class StmtKind : std::uint8_t {
    Block,
    Expr,
    VarDecl,
    Return,
    If,
    While,
    For,
};

/// Base class for every statement syntax node.
class Stmt : public NodeBase<StmtKind> {
protected:
    using NodeBase<StmtKind>::NodeBase;
};

using StmtPtr = std::unique_ptr<Stmt>;

/// A `{ ... }` block: an ordered list of statements. `span()` covers the
/// full range from the opening `{` through the closing `}`.
class BlockStmt final : public Stmt {
public:
    BlockStmt(std::vector<StmtPtr> statements, SourceSpan span) noexcept
        : Stmt(StmtKind::Block, span), statements_(std::move(statements)) {}

    const std::vector<StmtPtr>& statements() const noexcept { return statements_; }

private:
    std::vector<StmtPtr> statements_;
};

/// Every "body" slot in the language (function body, if/while/for
/// bodies) is grammatically required to be a literal block, never a
/// bare statement - so those slots are typed as BlockPtr rather than
/// the more general StmtPtr.
using BlockPtr = std::unique_ptr<BlockStmt>;

/// A bare expression used as a statement, e.g. `print(x)` or `x = value`
/// on its own line.
class ExprStmt final : public Stmt {
public:
    ExprStmt(ExprPtr expr, SourceSpan span) noexcept : Stmt(StmtKind::Expr, span), expr_(std::move(expr)) {}

    const Expr& expr() const noexcept { return *expr_; }

private:
    ExprPtr expr_;
};

/// Whether a VarDeclStmt was introduced with `let` (Immutable) or `mut`
/// (Mutable). One node covers both, rather than separate LetStmt/MutStmt
/// classes - the two forms differ only in this flag (GRAMMAR.md §21).
enum class BindingKind : std::uint8_t {
    Immutable,
    Mutable,
};

/// A variable declaration: `let name [: type] = initializer` or
/// `mut name [: type] = initializer`. The initializer is grammatically
/// required (GRAMMAR.md §21); the type annotation is optional. Neither
/// the type annotation nor the initializer is validated against the
/// other here - that is semantic analysis's job, not the parser's.
class VarDeclStmt final : public Stmt {
public:
    VarDeclStmt(BindingKind binding, Identifier name, TypeSyntaxPtr type, ExprPtr initializer,
                SourceSpan span) noexcept
        : Stmt(StmtKind::VarDecl, span),
          binding_(binding),
          name_(name),
          type_(std::move(type)),
          initializer_(std::move(initializer)) {}

    BindingKind binding() const noexcept { return binding_; }
    const Identifier& name() const noexcept { return name_; }

    /// nullptr when the declaration has no `: type` annotation
    /// (GRAMMAR.md §21: the annotation is optional).
    const TypeSyntax* type() const noexcept { return type_.get(); }

    const Expr& initializer() const noexcept { return *initializer_; }

private:
    BindingKind binding_;
    Identifier name_;
    TypeSyntaxPtr type_;
    ExprPtr initializer_;
};

/// A `return` statement: `return` or `return expression`
/// (GRAMMAR.md §22).
class ReturnStmt final : public Stmt {
public:
    ReturnStmt(ExprPtr value, SourceSpan span) noexcept : Stmt(StmtKind::Return, span), value_(std::move(value)) {}

    /// nullptr for a bare `return` with no expression.
    const Expr* value() const noexcept { return value_.get(); }

private:
    ExprPtr value_;
};

/// One `if`/`else if` branch: a condition and the block executed when
/// it is true. Plain helper record (no Kind, no virtual destructor),
/// like Param - not itself a polymorphic AST node. `span` covers the
/// branch's own `if`/`else if` keyword(s) through its body's closing
/// `}`; this is not derivable from `condition`/`body` alone since
/// neither child's own span includes the leading keyword(s). For the
/// initial branch, `span` begins at `if`; for an `else if` branch,
/// `span` begins at `else` (not the following `if`), so it covers the
/// complete `else if ... { }` source form.
struct IfBranch {
    ExprPtr condition;
    BlockPtr body;
    SourceSpan span;
};

/// The final `else` clause of an IfStmt, with no condition of its own.
/// Plain helper record, like IfBranch - not a polymorphic AST node (no
/// ElseStmt). `span` covers the `else` keyword through the body's
/// closing `}`; storing this separately (rather than a bare BlockPtr)
/// preserves the exact source location of the final `else` keyword,
/// which the body's own span alone does not cover.
struct ElseClause {
    BlockPtr body;
    SourceSpan span;
};

/// An `if` statement: an initial `if` branch, zero or more `else if`
/// branches, and an optional final `else` clause. Uses a flat
/// std::vector<IfBranch> rather than recursive nested IfStmt nodes -
/// branches[0] is the initial `if`, branches[1..] are each `else if` in
/// source order; no ElseIfStmt node exists purely to encode the source
/// spelling `else if`. `elseClause()` is std::nullopt when there is no
/// final `else`. Conditions are arbitrary expressions; the parser
/// performs no truthiness/type validation (e.g. `if 42 { }` parses
/// successfully - that is semantic analysis's job, not the parser's).
class IfStmt final : public Stmt {
public:
    IfStmt(std::vector<IfBranch> branches, std::optional<ElseClause> elseClause, SourceSpan span) noexcept
        : Stmt(StmtKind::If, span), branches_(std::move(branches)), elseClause_(std::move(elseClause)) {}

    const std::vector<IfBranch>& branches() const noexcept { return branches_; }
    const std::optional<ElseClause>& elseClause() const noexcept { return elseClause_; }

private:
    std::vector<IfBranch> branches_;
    std::optional<ElseClause> elseClause_;
};

/// A `while` statement: `while condition { body }`. The condition is an
/// arbitrary expression; no truthiness/type validation happens here.
class WhileStmt final : public Stmt {
public:
    WhileStmt(ExprPtr condition, BlockPtr body, SourceSpan span) noexcept
        : Stmt(StmtKind::While, span), condition_(std::move(condition)), body_(std::move(body)) {}

    const Expr& condition() const noexcept { return *condition_; }
    const BlockStmt& body() const noexcept { return *body_; }

private:
    ExprPtr condition_;
    BlockPtr body_;
};

/// A `for` statement: `for variable in iterable { body }`. `iterable`
/// is an arbitrary expression (e.g. the existing BinaryExpr{Range} for
/// `0..10`, or a bare identifier for `for value in values`) - the
/// parser never assumes it is a range. No destructuring/pattern support
/// (GRAMMAR.md §25: a single identifier): the loop variable is a plain
/// Identifier, span-only exactly like every other name in this AST.
class ForStmt final : public Stmt {
public:
    ForStmt(Identifier variable, ExprPtr iterable, BlockPtr body, SourceSpan span) noexcept
        : Stmt(StmtKind::For, span), variable_(variable), iterable_(std::move(iterable)), body_(std::move(body)) {}

    const Identifier& variable() const noexcept { return variable_; }
    const Expr& iterable() const noexcept { return *iterable_; }
    const BlockStmt& body() const noexcept { return *body_; }

private:
    Identifier variable_;
    ExprPtr iterable_;
    BlockPtr body_;
};

} // namespace kai::ast
