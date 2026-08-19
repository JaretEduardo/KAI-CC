#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/ast/TypeSyntax.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kai::ast {

/// This vocabulary covers only the Stmt node kinds implemented so far.
/// Extend it as later parser milestones add IfStmt, WhileStmt, ForStmt.
enum class StmtKind : std::uint8_t {
    Block,
    Expr,
    VarDecl,
    Return,
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

} // namespace kai::ast
