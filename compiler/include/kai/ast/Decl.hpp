#pragma once

#include "kai/ast/Node.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kai::ast {

/// This vocabulary covers only the Decl node kinds implemented so far.
/// Extend it as later parser milestones add StructDecl, EnumDecl,
/// UseDecl.
enum class DeclKind : std::uint8_t {
    Function,
};

/// Base class for every top-level declaration syntax node.
class Decl : public NodeBase<DeclKind> {
protected:
    using NodeBase<DeclKind>::NodeBase;
};

using DeclPtr = std::unique_ptr<Decl>;

/// A single function parameter: `name: type`.
///
/// Not itself a polymorphic AST node (no Kind, no virtual destructor) -
/// a plain helper record, like Identifier, stored by value in
/// FunctionDecl::params(). The TypeSyntax it owns is still
/// unique_ptr-managed for a uniform ownership model.
struct Param {
    Identifier name;
    TypeSyntaxPtr type;
    SourceSpan span;
};

/// A function declaration: `[pub] fn name(params...) [-> type] { body }`.
class FunctionDecl final : public Decl {
public:
    FunctionDecl(bool isPublic, Identifier name, std::vector<Param> params, TypeSyntaxPtr returnType,
                 BlockPtr body, SourceSpan span) noexcept
        : Decl(DeclKind::Function, span),
          isPublic_(isPublic),
          name_(name),
          params_(std::move(params)),
          returnType_(std::move(returnType)),
          body_(std::move(body)) {}

    bool isPublic() const noexcept { return isPublic_; }
    const Identifier& name() const noexcept { return name_; }
    const std::vector<Param>& params() const noexcept { return params_; }

    /// nullptr when the function has no `-> type` (GRAMMAR.md §9: the
    /// return type is optional).
    const TypeSyntax* returnType() const noexcept { return returnType_.get(); }

    const BlockStmt& body() const noexcept { return *body_; }

private:
    bool isPublic_;
    Identifier name_;
    std::vector<Param> params_;
    TypeSyntaxPtr returnType_;
    BlockPtr body_;
};

} // namespace kai::ast
