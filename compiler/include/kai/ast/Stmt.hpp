#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kai::ast {

/// This vocabulary covers only the Stmt node kinds implemented so far.
/// Extend it as later parser milestones add VarDeclStmt, ReturnStmt,
/// IfStmt, WhileStmt, ForStmt.
enum class StmtKind : std::uint8_t {
    Block,
    Expr,
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

} // namespace kai::ast
