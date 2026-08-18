#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/source/SourceLocation.hpp"

#include <vector>

namespace kai::ast {

/// The root of one parsed .kai file: an ordered list of top-level
/// declarations.
///
/// SourceFile is not itself a Decl/Stmt/Expr/TypeSyntax node - it is a
/// singleton shape (never stored in a heterogeneous collection, never
/// downcast from a common base), so it has no Kind and no virtual
/// destructor, the same way Identifier does not.
///
/// SourceFile does not resolve a filesystem module path (e.g.
/// `src/net/http.kai` -> `net.http`); that derivation belongs to a
/// later build/module-resolution layer, not the parser.
class SourceFile {
public:
    SourceFile(FileId file, std::vector<DeclPtr> declarations, SourceSpan span) noexcept
        : file_(file), declarations_(std::move(declarations)), span_(span) {}

    FileId file() const noexcept { return file_; }
    const std::vector<DeclPtr>& declarations() const noexcept { return declarations_; }
    SourceSpan span() const noexcept { return span_; }

private:
    FileId file_;
    std::vector<DeclPtr> declarations_;
    SourceSpan span_;
};

} // namespace kai::ast
