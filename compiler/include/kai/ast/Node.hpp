#pragma once

#include "kai/source/SourceLocation.hpp"

namespace kai::ast {

/// A name reference: just the source span of the identifier token.
///
/// Identifier is not itself a polymorphic AST node (no Kind, no virtual
/// destructor) - it is a small, span-only helper embedded inside the
/// nodes that need a name (IdentifierExpr, Param, FunctionDecl, ...).
/// The name's text is always recoverable through
/// SourceManager::text(identifier.span); it is never cached here.
struct Identifier {
    SourceSpan span;
};

/// Shared base for one AST node category (Expr, Stmt, Decl, TypeSyntax).
///
/// Every node stores its own Kind and its own complete SourceSpan
/// directly - spans are never derived from a child node, even when a
/// node wraps exactly one child. `kind()` is safe for checked-kind +
/// static_cast downcasting instead of RTTI. The constructor is
/// protected so only a derived leaf class can choose its Kind, and it
/// does so by hard-coding the value in its own constructor rather than
/// accepting a Kind parameter from the caller.
template <typename Kind>
class NodeBase {
public:
    virtual ~NodeBase() = default;

    Kind kind() const noexcept { return kind_; }
    SourceSpan span() const noexcept { return span_; }

protected:
    NodeBase(Kind kind, SourceSpan span) noexcept : kind_(kind), span_(span) {}

private:
    Kind kind_;
    SourceSpan span_;
};

} // namespace kai::ast
