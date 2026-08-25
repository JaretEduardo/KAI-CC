#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticInspector.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceManager.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kai::semantic {

/// SEMANTIC INSPECTION MILESTONE 3: one OTHER function in a direct call
/// relationship, plus every call site involved (M3 spec §11: a caller
/// may call the same callee more than once - this groups by semantic
/// function while retaining every individual call site, rather than
/// losing that information or emitting one edge per site).
struct CallSiteGroup {
    SemanticSymbolInfo function;
    /// The callee-identifier range of each call site (M3 spec §2 - the
    /// SAME convention M2 uses for a function reference), in SOURCE
    /// order.
    std::vector<InspectionRange> callSites;
};

/// The result of a callers/callees query: the QUERIED function (or
/// std::nullopt if the position does not resolve to a function - M3 spec
/// §6: pointing at a Local/Parameter/nothing is a valid, successful
/// query, never an error), plus its direct relations, grouped per
/// CallSiteGroup's own contract. `relations` is always empty when
/// `function` is std::nullopt.
struct CallRelationResult {
    std::optional<SemanticSymbolInfo> function;
    std::vector<CallSiteGroup> relations;
};

/// One function's outgoing direct call edges, for the whole-file call
/// graph.
struct CallGraphNode {
    SemanticSymbolInfo function;
    /// Always present, even when empty (M3 spec §20: a zero-callee
    /// function is still a meaningful graph node, never omitted).
    std::vector<CallSiteGroup> callees;
};

/// The complete direct (non-transitive) call graph of one file.
struct CallGraph {
    /// Every user function declared in the file, in SOURCE declaration
    /// order - never alphabetical, never omitted for having no callees.
    std::vector<CallGraphNode> functions;
};

/// SEMANTIC INSPECTION MILESTONE 3: a reusable, position-based DIRECT
/// call-relationship query layer, the M3 counterpart to M2's
/// SemanticQuery - same architecture, same precondition (`model` already
/// reflects a successful SemanticAnalyzer + TypeChecker +
/// ControlFlowAnalyzer run over `file`), same public-identity discipline
/// (position in, SemanticSymbolInfo out - semantic::SymbolId is used
/// internally only, during index construction and lookup, and is NEVER
/// returned to a caller in any form).
///
/// A direct call edge exists ONLY for a `CallExpr` whose direct (or
/// transparently-parenthesized) callee resolves, via
/// `SemanticModel::resolution()` - never identifier text - to a
/// `SymbolKind::Function` symbol (M3 spec §2). A call resolving to
/// `SymbolKind::Builtin` (e.g. an un-shadowed `print`) is deliberately
/// EXCLUDED from the graph - a Builtin has no user source declaration to
/// be a graph node (M3 spec §3); a user declaration that shadows a
/// prelude name (e.g. a user's own `fn print`) resolves to that user
/// Function exactly like any other call, with no special-casing needed
/// here (name resolution itself already decided that).
///
/// DIRECT only (M3 spec §4): `callees(main)` where `main` calls `a` and
/// `a` calls `b` is `[a]`, never `[a, b]`. This class performs no
/// transitive closure - it only ever records edges it observes directly
/// during its own single AST traversal.
///
/// Deliberately its OWN focused traversal rather than an extension of
/// SemanticQuery's occurrence index (M3 spec §8): unlike SemanticQuery,
/// this class never needs to track Local/Parameter declarations or
/// non-call identifier uses at all - only function declarations and
/// CallExpr call sites - so a small dedicated walker is genuinely
/// smaller and cleaner than threading call-edge bookkeeping through
/// SemanticQuery's broader occurrence model. The one piece of real
/// "declaration -> tooling symbol info" duplication this would otherwise
/// cause is eliminated via the shared `buildFunctionSymbolInfo()` helper
/// (SemanticInspector.hpp, M3 spec §30).
///
/// Never touches argv, stdout, stderr, or JSON text (see
/// kai::cli::runCallQueryCommand for the CLI/JSON consumer).
class SemanticCallQuery {
public:
    SemanticCallQuery(const SourceManager& sources, const SemanticModel& model, const ast::SourceFile& file);

    /// Every function that DIRECTLY calls the function at `position`,
    /// grouped by caller in the CALLER's OWN source declaration order
    /// (M3 spec §12) - never call-occurrence order.
    CallRelationResult findCallers(InspectionPosition position) const;

    /// Every function the function at `position` DIRECTLY calls, grouped
    /// by callee in FIRST-CALL-OCCURRENCE order within the queried
    /// function's body (M3 spec §12).
    CallRelationResult findCallees(InspectionPosition position) const;

    /// The complete direct call graph of the whole file.
    CallGraph directCallGraph() const;

private:
    /// Internal only - `caller`/`callee` are NEVER exposed to a caller.
    struct CallEdge {
        SymbolId caller;
        SymbolId callee;
        InspectionRange callSite;
    };

    void indexFile(const ast::SourceFile& file);
    void indexFunction(const ast::FunctionDecl& fn);
    void indexBlock(const ast::BlockStmt& block, SymbolId enclosingFunction);
    void indexStatement(const ast::Stmt& stmt, SymbolId enclosingFunction);
    void indexExpr(const ast::Expr& expr, SymbolId enclosingFunction);

    std::optional<SymbolId> resolveFunctionAt(InspectionPosition position) const;
    const SemanticSymbolInfo* functionInfoFor(SymbolId id) const;
    std::vector<CallSiteGroup> groupCalleesOf(SymbolId caller) const;

    const SourceManager& sources_;
    const SemanticModel& model_;

    /// Every user function declared in the file, in source order.
    /// Linear-scan-by-SymbolId, same rationale as every other SymbolId-
    /// keyed table in this compiler (SymbolId exposes no public hash/
    /// ordering, and a handful of functions per file makes a linear scan
    /// the smallest useful representation).
    std::vector<std::pair<SymbolId, SemanticSymbolInfo>> functions_;
    /// Every direct call edge, in traversal (source) order.
    std::vector<CallEdge> edges_;
};

} // namespace kai::semantic
