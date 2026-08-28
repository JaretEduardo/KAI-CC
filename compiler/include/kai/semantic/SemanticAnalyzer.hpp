#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceManager.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kai::semantic {

/// Semantic Foundation Phase 3B: adds IdentifierExpr *use* resolution -
/// a lexical lookup chain (innermost scope through file scope through an
/// outermost prelude scope), the print/panic/assert Builtin prelude, and
/// UnknownIdentifier - on top of Phase 3A's declaration/scope
/// infrastructure.
///
/// Still explicitly NOT implemented: expression typing of any kind
/// (literal inference, operator checking, call-signature checking,
/// assignment/mutability validation, return/condition validation,
/// member/reference/Result semantics). Every expression is now visited,
/// but purely for the identifiers it contains - MemberExpr::member() is
/// deliberately never looked up lexically (see analyzeExpr()'s Member
/// case).
///
/// SemanticAnalyzer keeps no state between calls: every analyze() call
/// builds its SemanticModel, its file-scope table, and its prelude scope
/// entirely from local state, so one SemanticAnalyzer instance may
/// safely analyze multiple, independent SourceFiles in sequence with no
/// leakage between calls - including no shared/duplicated Builtin
/// symbols, since each call's Builtins live in that call's own
/// SemanticModel.
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
    /// their FunctionSignature (Phase 2). Pass 2 then walks each
    /// function's body: declaring Parameter/Local/for-variable symbols
    /// with correct lexical scope boundaries (Phase 3A), and now also
    /// resolving every IdentifierExpr *use* against the lexical lookup
    /// chain (Phase 3B).
    SemanticModel analyze(const ast::SourceFile& file);

private:
    /// A single lexical scope: the set of names declared directly in it,
    /// mapped to their Symbol. Declaring into a Scope only ever checks
    /// that one Scope for a duplicate - it never walks outward; outward
    /// walking (name *lookup*) is ScopeStack's job, below. Each
    /// genuinely nested scope is a fresh, independent Scope value,
    /// created where the traversal enters it (an ordinary local
    /// variable, not heap-allocated or pooled).
    using Scope = std::unordered_map<std::string, SymbolId>;

    /// The chain of scopes *enclosing* whatever scope is currently being
    /// populated/analyzed, ordered outermost-first (prelude, then file,
    /// then each nested scope in turn) - the currently-active scope
    /// itself is passed alongside as a separate `const Scope&`/`Scope&`,
    /// never stored in this stack. Lookup (lookupIdentifier()) checks
    /// the current scope first, then walks this stack back-to-front
    /// (innermost enclosing to outermost/prelude) - see #2's required
    /// order. Entering a nested scope pushes the *old* current scope's
    /// address onto this stack for the duration of that nested
    /// traversal, then pops it back off - the classic push/analyze/pop
    /// shape, not a rebuilt-and-copied stack per nesting level. Every
    /// Scope whose address is ever pushed here is a real local variable
    /// still on the call stack for as long as its address remains
    /// pushed, so no pointer here ever outlives what it points to.
    using ScopeStack = std::vector<const Scope*>;

    // --- Pass 1: top-level declaration collection (Phase 2, unchanged) ---

    void collectTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model,
                                     std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    void collectFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model,
                              std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    FunctionSignature resolveFunctionSignature(const ast::FunctionDecl& fn, SemanticModel& model) const;

    /// Exhaustive over ast::TypeSyntaxKind, no `default:`: Named
    /// resolves to a primitive Type or Type::error() (with an
    /// UnknownType SemanticError); Unit resolves to Type::unit(); Array
    /// resolves to a real fixed-size array Type (KAI LANGUAGE M7A - see
    /// resolveArrayTypeSyntax()); Reference/Slice/Generic all resolve to
    /// Type::unresolved() with no SemanticError, since this phase does
    /// not model those semantic shapes yet (see Type.hpp's Unresolved-
    /// vs-Error distinction, and SemanticAnalyzer.cpp for the exact
    /// rationale).
    Type resolveTypeSyntax(const ast::TypeSyntax& type, SemanticModel& model) const;

    Type resolveNamedTypeSyntax(const ast::NamedTypeSyntax& type, SemanticModel& model) const;

    /// KAI LANGUAGE M7A: resolves `[T; N]` to a real, interned,
    /// structural array Type. `T` is resolved recursively through this
    /// SAME resolveTypeSyntax() dispatch (so a still-deferred element
    /// shape, e.g. `[[i32]; 3]`'s slice element, or an already-broken
    /// one, e.g. `[Foo; 3]` with unknown `Foo`, propagates through
    /// exactly like every other Unresolved/Error propagation in this
    /// file - no new diagnostic is invented here for a problem the
    /// recursive resolution already reported). `N` is decoded from the
    /// grammar-guaranteed integer-literal length expression
    /// (ArrayTypeSyntax::length()'s own doc comment) and the result is
    /// canonicalized via `model`'s own compound-type interner
    /// (SemanticModel::internArray()) - never a fabricated ad hoc Type.
    Type resolveArrayTypeSyntax(const ast::ArrayTypeSyntax& type, SemanticModel& model) const;

    // --- Pass 2: function-body declaration/scope/name analysis ---

    /// Creates exactly print/panic/assert as SymbolKind::Builtin symbols
    /// (STANDARD_LIBRARY.md's committed initial prelude) with no
    /// signature and no declarationSymbol() mapping - there is no AST
    /// declaration to point one at - and returns the Scope containing
    /// them. Called once per analyze() call, using that call's own
    /// model, specifically so Builtins from one analyze() call are never
    /// shared with another (see this class's own leakage note above).
    Scope buildPreludeScope(SemanticModel& model) const;

    void analyzeTopLevelDeclarationBody(const ast::Decl& decl, SemanticModel& model, const Scope& preludeScope,
                                         const Scope& fileScope) const;

    /// Creates the one lexical scope a function contributes (parameters
    /// and the outermost body block share it - see Scope's own comment),
    /// declares each parameter using the type ALREADY resolved by Pass
    /// 1's FunctionSignature (never re-resolving the parameter
    /// TypeSyntax), then analyzes the body's contents in that same
    /// scope, with the lookup chain seeded as [prelude, file].
    void analyzeFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model, const Scope& preludeScope,
                              const Scope& fileScope) const;

    /// Declares one name into `scope`: on a duplicate (a name already
    /// present in `scope`), emits DuplicateSymbol (primary = this
    /// identifier's span, related = the original symbol's declaredAt)
    /// and leaves the scope entry pointing at the original ("first
    /// declaration wins" - so later lookups of this name keep resolving
    /// to the original, never the duplicate); either way, `identifier`'s
    /// own Symbol and declarationSymbol() mapping are still created, so
    /// every syntactically-present declaration - including a duplicate -
    /// remains inspectable.
    SymbolId declareInScope(Scope& scope, SymbolKind kind, const ast::Identifier& identifier, bool isMutable,
                             Type type, SemanticModel& model) const;

    /// Analyzes `block`'s statements directly in `scope` - the block
    /// does NOT get its own new child scope. Used for the two cases that
    /// deliberately reuse an already-created scope: a function's
    /// outermost body block, and a for-loop's outermost body block.
    void analyzeBlockContents(const ast::BlockStmt& block, Scope& scope, ScopeStack& scopeStack,
                               SemanticModel& model) const;

    /// Pushes `enclosingScope`'s address onto `scopeStack`, creates a
    /// fresh child Scope, analyzes `block`'s contents in it, then pops.
    /// Used for every *genuinely* nested block: if/else-if/else bodies,
    /// while bodies, and a bare `{ ... }` block statement.
    void analyzeNestedBlock(const ast::BlockStmt& block, const Scope& enclosingScope, ScopeStack& scopeStack,
                             SemanticModel& model) const;

    /// Exhaustive over ast::StmtKind, no `default:`. VarDecl/If/While/For
    /// affect declarations or scope structure; Block is genuinely
    /// nested; Expr/Return now feed their (optional) expression into
    /// analyzeExpr() for name resolution only.
    void analyzeStatement(const ast::Stmt& stmt, Scope& scope, ScopeStack& scopeStack, SemanticModel& model) const;

    /// Resolves the (optional) type annotation using the same resolver
    /// Pass 1 uses for signatures, analyzes the initializer expression
    /// against `scope`/`scopeStack` AS THEY EXIST RIGHT NOW, and only
    /// then declares the local into `scope` - in that order, so a new
    /// binding is never visible inside its own initializer (`let x = x`
    /// resolves the RHS against any outer `x`, or UnknownIdentifier if
    /// there is none).
    void declareLocal(const ast::VarDeclStmt& varDecl, Scope& scope, ScopeStack& scopeStack,
                       SemanticModel& model) const;

    /// Each branch's condition is analyzed in the CURRENT scope (not the
    /// branch body's own scope); each branch body (including `else`)
    /// then gets its own fresh, sibling child scope - never shared with
    /// another branch or with the condition.
    void analyzeIfStmt(const ast::IfStmt& ifStmt, const Scope& scope, ScopeStack& scopeStack,
                        SemanticModel& model) const;

    /// The condition is analyzed in the current scope; the body gets a
    /// fresh child scope.
    void analyzeWhileStmt(const ast::WhileStmt& whileStmt, const Scope& scope, ScopeStack& scopeStack,
                           SemanticModel& model) const;

    /// Exact required order: the iterable is analyzed against the
    /// CURRENT scope/stack *before* the loop variable exists in any
    /// scope (so `for x in x` resolves the iterable `x` outward, never
    /// to the variable being declared); only then is the for-construct's
    /// one fresh child scope created, with the loop variable declared
    /// into it and the body's contents analyzed directly in that same
    /// scope (not a further-nested one).
    void analyzeForStmt(const ast::ForStmt& forStmt, const Scope& scope, ScopeStack& scopeStack,
                         SemanticModel& model) const;

    /// name -> SymbolId if `name` is found in `scope` itself or anywhere
    /// in `scopeStack`, walked innermost-enclosing-first; std::nullopt
    /// otherwise. Pure lookup: never mutates anything, never touches
    /// SemanticModel.
    std::optional<SymbolId> lookupIdentifier(const std::string& name, const Scope& scope,
                                              const ScopeStack& scopeStack) const;

    /// Resolves one identifier *use*: on a match, records
    /// IdentifierExpr* -> SymbolId via SemanticModel::recordResolution();
    /// otherwise emits UnknownIdentifier (primarySpan = the identifier's
    /// own span, no relatedSpan) and records no resolution at all for
    /// it. Never fabricates a Symbol for an unresolved name.
    void analyzeIdentifierExpr(const ast::IdentifierExpr& expr, const Scope& scope, const ScopeStack& scopeStack,
                                SemanticModel& model) const;

    /// Exhaustive over ast::ExprKind, no `default:`. Visits every
    /// expression purely for the IdentifierExpr uses it contains - never
    /// infers or stores a semantic Type for any expression. MemberExpr
    /// only visits its object; `member` is syntactic metadata resolved
    /// by (future) type-driven member lookup, never lexical scope
    /// lookup.
    void analyzeExpr(const ast::Expr& expr, const Scope& scope, const ScopeStack& scopeStack,
                      SemanticModel& model) const;

    const SourceManager& sources_;
};

} // namespace kai::semantic
