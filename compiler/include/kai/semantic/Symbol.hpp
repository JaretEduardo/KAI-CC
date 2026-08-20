#pragma once

#include "kai/semantic/Type.hpp"
#include "kai/source/SourceLocation.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace kai::semantic {

class SemanticModel;

/// Opaque handle to a Symbol owned by a SemanticModel.
///
/// SymbolId values are only ever produced by SemanticModel. Callers
/// should treat them as opaque tokens and never construct or index them
/// manually - the same contract kai::FileId already establishes for
/// SourceManager. Unlike a raw Symbol*, a SymbolId stays valid even if
/// the backing storage reallocates: it never points into that storage
/// directly, it is resolved back to a Symbol& through
/// SemanticModel::symbol() instead.
class SymbolId {
public:
    constexpr SymbolId() noexcept = default;

    constexpr bool isValid() const noexcept { return id_ != kInvalidId; }

    friend constexpr bool operator==(SymbolId lhs, SymbolId rhs) noexcept = default;

private:
    friend class SemanticModel;

    constexpr explicit SymbolId(std::uint32_t id) noexcept : id_(id) {}

    constexpr std::uint32_t rawId() const noexcept { return id_; }

    static constexpr std::uint32_t kInvalidId = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t id_ = kInvalidId;
};

/// This vocabulary covers every kind of declared name this milestone
/// represents. Extend it as later phases add further kinds - none yet.
enum class SymbolKind : std::uint8_t {
    Function,
    Parameter,
    Local,
    Builtin,
};

/// A semantic function signature - NOT a function-as-value Type. KAI 0.1
/// does not require first-class functions (TYPE_SYSTEM.md §42), so there
/// is no TypeKind::Function; a signature is its own small aggregate
/// instead. parameterTypes/returnType may individually be
/// Type::unresolved() for a syntactic type shape this milestone does not
/// yet model (see Type.hpp) - never Type::error() unless semantic
/// resolution genuinely failed for that specific position.
struct FunctionSignature {
    std::vector<Type> parameterTypes;
    Type returnType;
};

/// One declared name and everything known about it so far.
///
/// Symbol intentionally carries no borrow/ownership state and no HIR
/// references - those belong to later phases, once the concepts they
/// would describe (borrow-checking, HIR) actually exist.
struct Symbol {
    SymbolKind kind;

    /// Owned semantic name text, not an ast::Identifier: Symbol must be
    /// able to represent compiler-defined prelude entries (print, panic,
    /// assert, ...) that have no AST node and no source spelling to
    /// point a span-only Identifier at. For a source declaration,
    /// SemanticAnalyzer copies this from
    /// SourceManager::text(identifier.span()) at collection time. This
    /// is a separate concern from SemanticModel's
    /// `const ast::Identifier* -> SymbolId` declaration map: `name`
    /// answers "what is this symbol called", the declaration map
    /// answers "which symbol does this concrete source declaration
    /// correspond to" - the AST remains span-only either way.
    std::string name;

    /// The source location this symbol was declared at, or std::nullopt
    /// for a symbol with no source declaration at all (a builtin/prelude
    /// entry). Never a fabricated/sentinel SourceSpan standing in for
    /// "no real declaration" - std::optional expresses that state
    /// directly.
    std::optional<SourceSpan> declaredAt;

    /// Only meaningful for Local. Always false for Parameter - KAI 0.1's
    /// parameter grammar has no `mut` keyword (GRAMMAR.md §10), so a
    /// parameter binding itself can never be mutable regardless of
    /// whether its type is `&mut T`. Unused for Function/Builtin.
    bool isMutable = false;

    /// The declared/resolved semantic type. Meaningful for
    /// Parameter/Local; unused for Function/Builtin, whose type
    /// information (if any) lives in `signature` instead.
    Type type = Type::unresolved();

    /// Populated for Function symbols once a real signature has been
    /// resolved from their parsed parameter/return TypeSyntax.
    /// Deliberately left nullopt for Builtin symbols: STANDARD_LIBRARY.md
    /// commits builtin *names* (print, panic, assert, ...) to the
    /// initial prelude, but not their argument/return types, so
    /// inventing a signature here (e.g. "print: () -> ()") would record
    /// a false semantic fact. Actual builtin call signatures belong to a
    /// later milestone once they are genuinely designed. Unused for
    /// Parameter/Local.
    std::optional<FunctionSignature> signature;
};

} // namespace kai::semantic
