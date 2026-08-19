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

/// This vocabulary covers every Expr node kind implemented so far.
/// Extend it as later parser milestones add StructLiteralExpr, TryExpr.
enum class ExprKind : std::uint8_t {
    Literal,
    Identifier,
    Call,
    Paren,
    Unary,
    Binary,
    Assignment,
    ArrayLiteral,
    Index,
    Member,
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

/// A prefix unary operator: `-expr`, `!expr`, `&expr`, `&mut expr`.
enum class UnaryOperator : std::uint8_t {
    Negate,
    Not,
    Ref,
    RefMut,
};

/// `operatorSpan` covers only the operator syntax (e.g. `-`, or the full
/// `&mut` for a mutable reference); `span()` covers the whole expression
/// from the operator through the operand.
class UnaryExpr final : public Expr {
public:
    UnaryExpr(UnaryOperator op, SourceSpan operatorSpan, ExprPtr operand, SourceSpan span) noexcept
        : Expr(ExprKind::Unary, span), op_(op), operatorSpan_(operatorSpan), operand_(std::move(operand)) {}

    UnaryOperator op() const noexcept { return op_; }
    SourceSpan operatorSpan() const noexcept { return operatorSpan_; }
    const Expr& operand() const noexcept { return *operand_; }

private:
    UnaryOperator op_;
    SourceSpan operatorSpan_;
    ExprPtr operand_;
};

/// An infix binary operator. `Range` represents `..` (see GRAMMAR.md
/// §32): KAI 0.1 has no dedicated RangeExpr node, since a range is
/// syntactically just another binary operator at its own precedence
/// tier.
enum class BinaryOperator : std::uint8_t {
    Or,
    And,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Range,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
};

/// `operatorSpan` covers only the operator token (e.g. `+`, `==`, `..`);
/// `span()` covers the whole expression from `left` through `right`.
class BinaryExpr final : public Expr {
public:
    BinaryExpr(BinaryOperator op, SourceSpan operatorSpan, ExprPtr left, ExprPtr right, SourceSpan span) noexcept
        : Expr(ExprKind::Binary, span),
          op_(op),
          operatorSpan_(operatorSpan),
          left_(std::move(left)),
          right_(std::move(right)) {}

    BinaryOperator op() const noexcept { return op_; }
    SourceSpan operatorSpan() const noexcept { return operatorSpan_; }
    const Expr& left() const noexcept { return *left_; }
    const Expr& right() const noexcept { return *right_; }

private:
    BinaryOperator op_;
    SourceSpan operatorSpan_;
    ExprPtr left_;
    ExprPtr right_;
};

/// An assignment: `target = value`. `target` is an arbitrary Expr, not
/// restricted to IdentifierExpr - GRAMMAR.md §27's production
/// (`logical_or ["=" assignment]`) has no distinguished lvalue
/// nonterminal, and "assignment targets must be valid mutable locations"
/// is a semantic-analysis rule, not a parsing one. `operatorSpan` covers
/// only the `=` token; `span()` covers the whole expression from
/// `target` through `value`.
class AssignmentExpr final : public Expr {
public:
    AssignmentExpr(ExprPtr target, SourceSpan operatorSpan, ExprPtr value, SourceSpan span) noexcept
        : Expr(ExprKind::Assignment, span),
          operatorSpan_(operatorSpan),
          target_(std::move(target)),
          value_(std::move(value)) {}

    SourceSpan operatorSpan() const noexcept { return operatorSpan_; }
    const Expr& target() const noexcept { return *target_; }
    const Expr& value() const noexcept { return *value_; }

private:
    SourceSpan operatorSpan_;
    ExprPtr target_;
    ExprPtr value_;
};

/// `[elem, elem, ...]` - an array literal. No element-type inference or
/// uniformity checking happens here (semantic analysis's job): `[1,
/// "two", true]` constructs successfully at the syntax level. `span()`
/// covers the full `[...]` range, including an empty `[]`. No comma
/// spans are retained - CallExpr::arguments() doesn't track its own
/// separator commas either.
class ArrayLiteralExpr final : public Expr {
public:
    ArrayLiteralExpr(std::vector<ExprPtr> elements, SourceSpan span) noexcept
        : Expr(ExprKind::ArrayLiteral, span), elements_(std::move(elements)) {}

    const std::vector<ExprPtr>& elements() const noexcept { return elements_; }

private:
    std::vector<ExprPtr> elements_;
};

/// `object[index]`. `index` is an arbitrary expression syntactically
/// (`values[i + 1]` parses); whether `object` is indexable or `index`
/// is integer-typed is semantic analysis's job, not the parser's.
/// `span()` covers `object` through the closing `]`.
class IndexExpr final : public Expr {
public:
    IndexExpr(ExprPtr object, ExprPtr index, SourceSpan span) noexcept
        : Expr(ExprKind::Index, span), object_(std::move(object)), index_(std::move(index)) {}

    const Expr& object() const noexcept { return *object_; }
    const Expr& index() const noexcept { return *index_; }

private:
    ExprPtr object_;
    ExprPtr index_;
};

/// `object.member`. Member resolution (field vs. method vs. builtin, or
/// whether it exists at all) is semantic analysis's job - the parser
/// records only that `.identifier` syntax occurred. `member` stays a
/// span-only Identifier, like every other name in this AST - no string
/// is stored. `span()` covers `object` through the member identifier.
/// No separate `.` operator span: unlike BinaryExpr (14 possible
/// operators worth individually pinpointing), `.` is the only member-
/// access token, and `member().span` already pinpoints the interesting
/// part precisely.
class MemberExpr final : public Expr {
public:
    MemberExpr(ExprPtr object, Identifier member, SourceSpan span) noexcept
        : Expr(ExprKind::Member, span), object_(std::move(object)), member_(member) {}

    const Expr& object() const noexcept { return *object_; }
    const Identifier& member() const noexcept { return member_; }

private:
    ExprPtr object_;
    Identifier member_;
};

} // namespace kai::ast
