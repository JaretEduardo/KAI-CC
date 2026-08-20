#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceManager.hpp"

#include <string>
#include <unordered_map>

namespace kai::semantic {

/// Semantic Foundation Phase 3A: adds lexical declaration scopes,
/// Parameter/Local symbols, and duplicate detection within function
/// bodies, on top of Phase 2's top-level function collection.
///
/// Still explicitly NOT implemented: IdentifierExpr *use* resolution
/// (model.resolution() stays nullopt for every identifier use), the
/// print/panic/assert prelude, Builtin symbols, expression typing, call
/// checking, and return/condition/mutability validation. Phase 3A walks
/// function bodies structurally, deep enough to find declarations and
/// scope boundaries, and no deeper - it never inspects an expression's
/// content (ExprStmt bodies, ReturnStmt values, if/while conditions,
/// for-loop iterables, VarDeclStmt initializers are all skipped
/// entirely). That is Phase 3B.
///
/// SemanticAnalyzer keeps no state between calls: every analyze() call
/// builds its SemanticModel and every scope entirely from local state,
/// so one SemanticAnalyzer instance may safely analyze multiple,
/// independent SourceFiles in sequence with no leakage between calls.
class SemanticAnalyzer {
public:
    /// `sources` is stored non-owningly, matching Parser's own contract.
    /// It, and every ast::SourceFile later passed to analyze(), must
    /// outlive any SemanticModel this analyzer returns: SemanticModel
    /// stores AST pointer identity (see SemanticModel.hpp's own lifetime
    /// contract), and name resolution here reads source text through
    /// `sources`.
    explicit SemanticAnalyzer(const SourceManager& sources) noexcept;

    /// Pass 1 collects top-level ast::FunctionDecl symbols and resolves
    /// their FunctionSignature (Phase 2, unchanged). Pass 2 (Phase 3A)
    /// then walks each function's body for Parameter/Local declarations
    /// and lexical scope structure. No identifier *use* is resolved by
    /// either pass yet.
    SemanticModel analyze(const ast::SourceFile& file);

private:
    /// A single lexical scope: the set of names declared directly in it,
    /// mapped to their Symbol. Deliberately not a stack/chain of scopes:
    /// Phase 3A only ever needs to ask "does this name already exist in
    /// the scope currently being populated" (duplicate detection), never
    /// "does this name exist in some enclosing scope" (that is
    /// name *lookup*, which does not exist until Phase 3B resolves
    /// identifier uses). Each genuinely nested scope is therefore just a
    /// fresh, independent Scope value, threaded through the recursive
    /// body traversal via the ordinary C++ call stack - not stored in an
    /// explicit container. Phase 3B can add real chain-walking lookup
    /// later without disturbing this: it is an additive change (carry
    /// the enclosing scopes alongside, e.g. for a lookup helper), not a
    /// redesign of how scopes are created here.
    using Scope = std::unordered_map<std::string, SymbolId>;

    // --- Pass 1: top-level declaration collection (Phase 2, unchanged) ---

    void collectTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model,
                                     std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    void collectFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model,
                              std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    FunctionSignature resolveFunctionSignature(const ast::FunctionDecl& fn, SemanticModel& model) const;

    /// Exhaustive over ast::TypeSyntaxKind, no `default:`: Named
    /// resolves to a primitive Type or Type::error() (with an
    /// UnknownType SemanticError); Unit resolves to Type::unit();
    /// Reference/Slice/Array/Generic all resolve to Type::unresolved()
    /// with no SemanticError, since this phase does not model those
    /// semantic shapes yet (see Type.hpp's Unresolved-vs-Error
    /// distinction, and SemanticAnalyzer.cpp for the exact rationale).
    Type resolveTypeSyntax(const ast::TypeSyntax& type, SemanticModel& model) const;

    Type resolveNamedTypeSyntax(const ast::NamedTypeSyntax& type, SemanticModel& model) const;

    // --- Pass 2: function-body declaration/scope analysis (Phase 3A) ---

    void analyzeTopLevelDeclarationBody(const ast::Decl& decl, SemanticModel& model) const;

    /// Creates the one lexical scope a function contributes (parameters
    /// and the outermost body block share it - see Scope's own comment
    /// and GRAMMAR.md-independent policy notes in SemanticAnalyzer.cpp),
    /// declares each parameter using the type ALREADY resolved by Pass
    /// 1's FunctionSignature (never re-resolving the parameter
    /// TypeSyntax), then analyzes the body's contents in that same
    /// scope.
    void analyzeFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model) const;

    /// Declares one name into `scope`: on a duplicate (a name already
    /// present in `scope`), emits DuplicateSymbol (primary = this
    /// identifier's span, related = the original symbol's declaredAt)
    /// and leaves the scope entry pointing at the original ("first
    /// declaration wins"); either way, `identifier`'s own Symbol and
    /// declarationSymbol() mapping are still created, so every
    /// syntactically-present declaration - including a duplicate -
    /// remains inspectable.
    SymbolId declareInScope(Scope& scope, SymbolKind kind, const ast::Identifier& identifier, bool isMutable,
                             Type type, SemanticModel& model) const;

    /// Analyzes `block`'s statements directly in `scope` - the block
    /// does NOT get its own new child scope. Used for the two cases that
    /// deliberately reuse an already-created scope: a function's
    /// outermost body block, and a for-loop's outermost body block.
    void analyzeBlockContents(const ast::BlockStmt& block, Scope& scope, SemanticModel& model) const;

    /// Creates a fresh child Scope and analyzes `block`'s contents in
    /// it. Used for every *genuinely* nested block: if/else-if/else
    /// bodies, while bodies, and a bare `{ ... }` block statement.
    void analyzeNestedBlock(const ast::BlockStmt& block, SemanticModel& model) const;

    /// Exhaustive over ast::StmtKind, no `default:`. Only
    /// VarDecl/If/While/For/Block affect declarations or scope
    /// structure; Expr and Return are statement kinds whose *content* is
    /// never inspected in this phase (no expression traversal at all).
    void analyzeStatement(const ast::Stmt& stmt, Scope& scope, SemanticModel& model) const;

    /// Resolves the (optional) type annotation using the same resolver
    /// Pass 1 uses for signatures, then declares the local into `scope`.
    /// No initializer traversal (Phase 3B). The annotation is resolved
    /// before declaration specifically so Phase 3B can insert initializer
    /// analysis, unchanged, between the two steps: a new binding must
    /// never become visible inside its own initializer.
    void declareLocal(const ast::VarDeclStmt& varDecl, Scope& scope, SemanticModel& model) const;

    /// Each branch body (including `else`) gets its own fresh, sibling
    /// child scope - never shared with another branch. Conditions are
    /// not analyzed.
    void analyzeIfStmt(const ast::IfStmt& ifStmt, SemanticModel& model) const;

    /// The body gets a fresh child scope. The condition is not analyzed.
    void analyzeWhileStmt(const ast::WhileStmt& whileStmt, SemanticModel& model) const;

    /// The entire for-construct is one fresh child scope relative to its
    /// surrounding scope; the loop variable and the body's outermost
    /// declarations share that one scope (not two nested scopes). The
    /// iterable is not analyzed.
    void analyzeForStmt(const ast::ForStmt& forStmt, SemanticModel& model) const;

    const SourceManager& sources_;
};

} // namespace kai::semantic
