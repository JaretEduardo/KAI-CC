#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kai::ast {

/// The complete vocabulary of syntactic type forms (GRAMMAR.md §11). All
/// six values have concrete node classes.
enum class TypeSyntaxKind : std::uint8_t {
    Named,
    Reference,
    Array,
    Slice,
    Unit,
    Generic,
};

/// Base class for every type syntax node.
///
/// TypeSyntax represents the syntactic *shape* of a type as written in
/// source (e.g. `&mut [i32]`), never a resolved semantic type. Deciding
/// whether a name denotes a built-in primitive or a user-defined type,
/// or whether a bare slice is legal in a given position, is semantic
/// analysis's job, not the parser's.
class TypeSyntax : public NodeBase<TypeSyntaxKind> {
protected:
    using NodeBase<TypeSyntaxKind>::NodeBase;
};

using TypeSyntaxPtr = std::unique_ptr<TypeSyntax>;

/// A named type: a primitive (`i32`, `bool`, ...) or a user-defined name
/// (`String`, `User`, ...). These are syntactically identical - the
/// lexer/parser never distinguishes them, since primitive type names
/// lex as ordinary identifiers.
class NamedTypeSyntax final : public TypeSyntax {
public:
    NamedTypeSyntax(Identifier name, SourceSpan span) noexcept
        : TypeSyntax(TypeSyntaxKind::Named, span), name_(name) {}

    const Identifier& name() const noexcept { return name_; }

private:
    Identifier name_;
};

/// `()` as a type - the unit type (GRAMMAR.md §18). No fields. `span()`
/// covers `(` through `)`.
class UnitTypeSyntax final : public TypeSyntax {
public:
    explicit UnitTypeSyntax(SourceSpan span) noexcept : TypeSyntax(TypeSyntaxKind::Unit, span) {}
};

/// Whether a ReferenceTypeSyntax is `&T` (Immutable) or `&mut T`
/// (Mutable). A dedicated enum, not reused: BindingKind describes
/// binding mutability (`let`/`mut`), UnaryOperator::Ref/RefMut describes
/// the value-level `&`/`&mut` prefix operator, and this describes
/// reference-*type* mutability - three different syntactic concepts
/// that happen to share a two-state shape. Sharing one enum across them
/// would let e.g. a ReferenceTypeSyntax be constructed with
/// UnaryOperator::Negate, a nonsensical state the type system should
/// not even make representable.
enum class ReferenceMutability : std::uint8_t {
    Immutable,
    Mutable,
};

/// `&T` (mutability() == Immutable) or `&mut T` (mutability() ==
/// Mutable). No lifetime/borrow information is represented - KAI 0.1
/// has no lifetime syntax (MEMORY_MODEL.md §13).
///
/// `operatorSpan` covers only `&` (or the full `&mut`, Amp.begin ->
/// KwMut.end - whitespace-inclusive, since `&mut T` and `& mut T`
/// tokenize identically); `span()` covers the whole type from the
/// operator through `referent`. This mirrors UnaryExpr::operatorSpan
/// exactly, for the same reason: it preserves whether the source spelled
/// `&mut` or `& mut`, information not otherwise recoverable without
/// deriving it from surrounding whitespace at read time.
class ReferenceTypeSyntax final : public TypeSyntax {
public:
    ReferenceTypeSyntax(ReferenceMutability mutability, SourceSpan operatorSpan, TypeSyntaxPtr referent,
                         SourceSpan span) noexcept
        : TypeSyntax(TypeSyntaxKind::Reference, span),
          mutability_(mutability),
          operatorSpan_(operatorSpan),
          referent_(std::move(referent)) {}

    ReferenceMutability mutability() const noexcept { return mutability_; }
    SourceSpan operatorSpan() const noexcept { return operatorSpan_; }
    const TypeSyntax& referent() const noexcept { return *referent_; }

private:
    ReferenceMutability mutability_;
    SourceSpan operatorSpan_;
    TypeSyntaxPtr referent_;
};

/// `[T]` - a slice type, syntactically legal in any type position
/// (GRAMMAR.md §16). Whether a bare (non-referenced) slice is
/// semantically valid in a given position is semantic analysis's
/// concern - the parser does not reject it. `span()` covers `[` through
/// `]`.
class SliceTypeSyntax final : public TypeSyntax {
public:
    SliceTypeSyntax(TypeSyntaxPtr element, SourceSpan span) noexcept
        : TypeSyntax(TypeSyntaxKind::Slice, span), element_(std::move(element)) {}

    const TypeSyntax& element() const noexcept { return *element_; }

private:
    TypeSyntaxPtr element_;
};

/// `[T; N]` - a fixed-size array type. `length` is stored as an ExprPtr
/// for architectural consistency with every other value-producing AST
/// slot, but the Parser (Phase 2) only ever constructs it as
/// LiteralExpr(Integer): GRAMMAR.md §15 currently requires
/// `integer_literal` specifically, not a general expression. No numeric
/// decoding happens here or ever will at this layer. `span()` covers `[`
/// through `]`.
class ArrayTypeSyntax final : public TypeSyntax {
public:
    ArrayTypeSyntax(TypeSyntaxPtr element, ExprPtr length, SourceSpan span) noexcept
        : TypeSyntax(TypeSyntaxKind::Array, span), element_(std::move(element)), length_(std::move(length)) {}

    const TypeSyntax& element() const noexcept { return *element_; }
    const Expr& length() const noexcept { return *length_; }

private:
    TypeSyntaxPtr element_;
    ExprPtr length_;
};

/// `identifier "<" type_list ">"` - generic use-site syntax (GRAMMAR.md
/// §17), e.g. `Result<T, E>`, `Option<i32>`, `Buffer<&mut [f32]>`. No
/// generic *declaration* syntax exists yet (`fn identity<T>`, `struct
/// Box<T>` remain unsupported) - this node only represents a generic
/// type appearing in a type position. `name` is a bare Identifier, not a
/// nested TypeSyntaxPtr: GRAMMAR.md §17 requires the base to be exactly
/// `identifier`, not a recursive `type`. `arguments` holds at least one
/// element once the parser builds this node (§17's `type_list` has no
/// zero-argument form), but enforcing that is the parser's job, not this
/// node's. `span()` covers the base identifier through the closing `>`.
class GenericTypeSyntax final : public TypeSyntax {
public:
    GenericTypeSyntax(Identifier name, std::vector<TypeSyntaxPtr> arguments, SourceSpan span) noexcept
        : TypeSyntax(TypeSyntaxKind::Generic, span), name_(name), arguments_(std::move(arguments)) {}

    const Identifier& name() const noexcept { return name_; }
    const std::vector<TypeSyntaxPtr>& arguments() const noexcept { return arguments_; }

private:
    Identifier name_;
    std::vector<TypeSyntaxPtr> arguments_;
};

} // namespace kai::ast
