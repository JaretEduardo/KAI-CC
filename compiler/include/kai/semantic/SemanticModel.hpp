#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/semantic/Symbol.hpp"
#include "kai/source/SourceLocation.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kai::semantic {

/// This vocabulary covers every kind of semantic failure this milestone
/// represents. Extend it as later phases add further kinds - none yet.
enum class SemanticErrorKind : std::uint8_t {
    DuplicateSymbol,
    UnknownIdentifier,
    UnknownType,
    TypeMismatch,
    LiteralOutOfRange,
    InvalidUnaryOperand,
    InvalidBinaryOperands,
};

/// A minimal, message-free description of a semantic failure - the
/// semantic-analysis counterpart to parser::ParseError, but deliberately
/// not a reuse of it: ParseError's `actual`/`expected` TokenKind fields
/// describe a syntactic-failure shape with no meaning here (there is no
/// "expected token" for a duplicate symbol). No message string, no
/// diagnostic code, no severity, no diagnostics-renderer integration yet
/// - this is a temporary stand-in for a future Diagnostic, the same role
/// ParseError plays for the parser.
struct SemanticError {
    SemanticErrorKind kind;
    SourceSpan primarySpan;

    /// Set only when a second location is meaningful, e.g.
    /// DuplicateSymbol's original declaration site, or TypeMismatch's/
    /// LiteralOutOfRange's explicit annotation span.
    std::optional<SourceSpan> relatedSpan;

    /// Only meaningful for TypeMismatch (concrete expected type) and
    /// LiteralOutOfRange (target integer type); nullopt otherwise. The
    /// default member initializer (rather than relying on aggregate-init
    /// value-initialization alone) is what lets every existing 3-field
    /// SemanticError{...} call site in SemanticAnalyzer.cpp keep
    /// compiling with no -Wmissing-field-initializers warning.
    std::optional<Type> expectedType = std::nullopt;

    /// Only meaningful for TypeMismatch (concrete actual type); nullopt
    /// otherwise, including for LiteralOutOfRange.
    std::optional<Type> actualType = std::nullopt;
};

/// Forward-declared only so SemanticModel can grant it access to its
/// private mutators (see below) - not implemented in this phase.
class SemanticAnalyzer;

/// Forward-declared only so SemanticModel can grant it access to its
/// private expression/symbol type mutators (see below). TypeChecker runs
/// as a separate pass after SemanticAnalyzer, over the same SemanticModel.
class TypeChecker;

/// The result of semantic analysis on one SourceFile: every declared
/// Symbol, every identifier-use resolution, every declaration-identifier
/// association, and every SemanticError collected along the way.
///
/// SemanticModel never mutates the AST: all semantic information is
/// attached here, externally, keyed by the address of the AST node it
/// describes, rather than by extending ast::Expr/ast::Decl with semantic
/// fields.
///
/// AST identity: SemanticModel keys its maps on raw pointer identity -
/// `const ast::IdentifierExpr*` for identifier uses, `const
/// ast::Identifier*` for declaration names - never SourceSpan. Two
/// different AST nodes can legitimately share an identical SourceSpan in
/// this AST today (e.g. Parser.cpp's expression-statement path always
/// constructs an ExprStmt's span as exactly its wrapped Expr's own
/// span), which makes SourceSpan unsound as a lookup key. No NodeId is
/// introduced either: AST nodes in this compiler are heap-allocated via
/// unique_ptr and never relocated after construction, so their addresses
/// are already stable for the AST's entire lifetime - exactly the
/// property a map key needs, with no new AST infrastructure. The
/// declaration-Identifier map is deliberately keyed on `ast::Identifier`
/// (a plain, span-only, non-polymorphic struct embedded by value inside
/// FunctionDecl/Param/VarDeclStmt/ForStmt) rather than on each of those
/// containing node types individually, so one map handles every future
/// declaration category uniformly.
///
/// Lifetime contract: the ast::SourceFile (and everything it owns) used
/// to build this SemanticModel must remain alive for as long as this
/// SemanticModel is queried. SemanticModel stores addresses into that
/// tree; it never extends its lifetime.
class SemanticModel {
public:
    SemanticModel() = default;

    /// The SymbolId a given identifier *use* resolved to, or
    /// std::nullopt if it never resolved (e.g. an UnknownIdentifier
    /// error was recorded for it instead).
    std::optional<SymbolId> resolution(const ast::IdentifierExpr& expr) const {
        const auto it = identifierResolutions_.find(&expr);
        if (it == identifierResolutions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// The SymbolId a given *declaration* name resolves to. Covers every
    /// declaration-site ast::Identifier uniformly - FunctionDecl::name(),
    /// Param::name, VarDeclStmt::name(), ForStmt::variable() - without a
    /// separate map per declaration category.
    std::optional<SymbolId> declarationSymbol(const ast::Identifier& identifier) const {
        const auto it = declarationSymbols_.find(&identifier);
        if (it == declarationSymbols_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    const Symbol& symbol(SymbolId id) const {
        assert(id.isValid());
        return symbols_[id.rawId()];
    }

    const std::vector<SemanticError>& errors() const noexcept { return errors_; }

    /// The semantic Type recorded for `expr` by TypeChecker, or
    /// std::nullopt if `expr` has not been type-checked at all. Distinct
    /// from a recorded Type::unresolved()/Type::error() - see
    /// TypeChecker.hpp's class comment for the three-state contract this
    /// deliberately preserves.
    std::optional<Type> typeOf(const ast::Expr& expr) const {
        const auto it = expressionTypes_.find(&expr);
        if (it == expressionTypes_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    // Only a future SemanticAnalyzer populates a SemanticModel. Nothing
    // else - not even tests - should be able to construct an arbitrary
    // (possibly invalid) SemanticModel state through the public API;
    // friendship keeps that mutation surface to exactly the one
    // component responsible for producing correct semantic facts.
    friend class SemanticAnalyzer;

    /// TypeChecker is the only component allowed to record expression
    /// types or overwrite a symbol's inferred type, mirroring the same
    /// friendship-only-mutation contract SemanticAnalyzer already has
    /// above - never exposed as public mutable semantic state.
    friend class TypeChecker;

    SymbolId addSymbol(Symbol symbol) {
        symbols_.push_back(std::move(symbol));
        return SymbolId(static_cast<std::uint32_t>(symbols_.size() - 1));
    }

    void recordResolution(const ast::IdentifierExpr& expr, SymbolId id) {
        identifierResolutions_.emplace(&expr, id);
    }

    void recordDeclaration(const ast::Identifier& identifier, SymbolId id) {
        declarationSymbols_.emplace(&identifier, id);
    }

    void addError(SemanticError error) { errors_.push_back(std::move(error)); }

    void setExpressionType(const ast::Expr& expr, Type type) { expressionTypes_.insert_or_assign(&expr, type); }

    void setSymbolType(SymbolId id, Type type) {
        assert(id.isValid());
        symbols_[id.rawId()].type = type;
    }

    std::vector<Symbol> symbols_;
    std::unordered_map<const ast::IdentifierExpr*, SymbolId> identifierResolutions_;
    std::unordered_map<const ast::Identifier*, SymbolId> declarationSymbols_;
    std::vector<SemanticError> errors_;
    std::unordered_map<const ast::Expr*, Type> expressionTypes_;
};

} // namespace kai::semantic
