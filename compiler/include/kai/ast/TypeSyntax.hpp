#pragma once

#include "kai/ast/Node.hpp"

#include <cstdint>
#include <memory>

namespace kai::ast {

/// The full future vocabulary of syntactic type forms (GRAMMAR.md §11).
/// Only Named is implemented as a concrete node so far; the rest are
/// reserved for later parser milestones (Reference: &T / &mut T, Array:
/// [T; N], Slice: [T], Unit: (), Generic: Result<T, E> use-site syntax).
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

} // namespace kai::ast
