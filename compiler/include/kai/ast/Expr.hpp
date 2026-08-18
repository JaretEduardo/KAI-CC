#pragma once

#include "kai/ast/Node.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kai::ast {

/// The lexical category of a LiteralExpr. Mirrors the shape of a lexed
/// literal token only - no value is parsed or decoded from it (see
/// LiteralExpr).
enum class LiteralKind : std::uint8_t {
    Integer,
    Float,
    String,
    Char,
    Bool,
};

/// This vocabulary covers only the Expr node kinds implemented so far.
/// Extend it as later parser milestones add BinaryExpr, UnaryExpr,
/// AssignmentExpr, MemberExpr, IndexExpr, ArrayLiteralExpr,
/// StructLiteralExpr, TryExpr.
enum class ExprKind : std::uint8_t {
    Literal,
    Identifier,
    Call,
    Paren,
};

/// Base class for every expression syntax node.
///
/// Expr represents syntax only: it has no notion of a resolved type,
/// constant value, or symbol identity. Every concrete Expr stores its
/// own SourceSpan explicitly (see NodeBase); nothing is derived from a
/// child node.
class Expr : public NodeBase<ExprKind> {
protected:
    using NodeBase<ExprKind>::NodeBase;
};

using ExprPtr = std::unique_ptr<Expr>;

/// A literal token's syntactic shape, unparsed.
///
/// LiteralExpr never decodes its value: no integer conversion, no
/// floating-point conversion, no string escape decoding, no char
/// decoding. The literal's exact source text remains available through
/// SourceManager::text(span()). Value decoding is semantic-analysis
/// work, deferred until the lexical grammar for numbers/escapes is
/// finalized.
class LiteralExpr final : public Expr {
public:
    LiteralExpr(LiteralKind literalKind, SourceSpan span) noexcept
        : Expr(ExprKind::Literal, span), literalKind_(literalKind) {}

    LiteralKind literalKind() const noexcept { return literalKind_; }

private:
    LiteralKind literalKind_;
};

/// A bare name used as an expression, e.g. `value` in `print(value)`.
class IdentifierExpr final : public Expr {
public:
    IdentifierExpr(Identifier name, SourceSpan span) noexcept
        : Expr(ExprKind::Identifier, span), name_(name) {}

    const Identifier& name() const noexcept { return name_; }

private:
    Identifier name_;
};

/// A function call: `callee(arguments...)`.
///
/// `callee` is an arbitrary expression (not restricted to an
/// identifier) so that syntax such as `foo.bar()` composes naturally
/// once member-access expressions exist.
class CallExpr final : public Expr {
public:
    CallExpr(ExprPtr callee, std::vector<ExprPtr> arguments, SourceSpan span) noexcept
        : Expr(ExprKind::Call, span), callee_(std::move(callee)), arguments_(std::move(arguments)) {}

    const Expr& callee() const noexcept { return *callee_; }
    const std::vector<ExprPtr>& arguments() const noexcept { return arguments_; }

private:
    ExprPtr callee_;
    std::vector<ExprPtr> arguments_;
};

/// A parenthesized expression: `(inner)`.
///
/// Kept as an explicit node (rather than returning `inner` with a
/// widened span) so that grouping parentheses remain visible in the
/// AST for diagnostics/tooling. `span()` covers the full `(...)` range;
/// `inner().span()` covers only the wrapped expression. Semantic
/// analysis/HIR lowering may erase ParenExpr once grouping information
/// is no longer needed.
class ParenExpr final : public Expr {
public:
    ParenExpr(ExprPtr inner, SourceSpan span) noexcept
        : Expr(ExprKind::Paren, span), inner_(std::move(inner)) {}

    const Expr& inner() const noexcept { return *inner_; }

private:
    ExprPtr inner_;
};

} // namespace kai::ast
