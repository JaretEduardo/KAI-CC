#include "kai/semantic/SemanticCallQuery.hpp"

#include <algorithm>
#include <cassert>

namespace kai::semantic {

namespace {

// Half-open containment, identical convention/logic to SemanticQuery.cpp's
// own (anonymous-namespace, file-local) helper of the same shape - kept
// as a small, deliberately-duplicated structural utility rather than
// shared, the same way TypeChecker.cpp/LLVMExpressionLowering.cpp each
// carry their own tiny unwrapDirectCalleeIdentifier() (see below).
bool rangeContains(const InspectionRange& range, InspectionPosition position) {
    if (position.line < range.start.line || position.line > range.end.line) {
        return false;
    }
    if (range.start.line == range.end.line) {
        return position.line == range.start.line && position.column >= range.start.column &&
               position.column < range.end.column;
    }
    if (position.line == range.start.line) {
        return position.column >= range.start.column;
    }
    if (position.line == range.end.line) {
        return position.column < range.end.column;
    }
    return true;
}

// Structural-only unwrap, mirroring TypeChecker.cpp's/
// LLVMExpressionLowering.cpp's own identically-shaped
// unwrapDirectCalleeIdentifier() - never a semantic decision, just AST
// navigation to find the bare identifier (if any) a call's callee names,
// transparently through ParenExpr wrappers.
const ast::IdentifierExpr* unwrapDirectCalleeIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapDirectCalleeIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

} // namespace

SemanticCallQuery::SemanticCallQuery(const SourceManager& sources, const SemanticModel& model,
                                      const ast::SourceFile& file)
    : sources_(sources), model_(model) {
    indexFile(file);
}

const SemanticSymbolInfo* SemanticCallQuery::functionInfoFor(SymbolId id) const {
    for (const auto& [candidateId, info] : functions_) {
        if (candidateId == id) {
            return &info;
        }
    }
    return nullptr;
}

std::optional<SymbolId> SemanticCallQuery::resolveFunctionAt(InspectionPosition position) const {
    // A function's OWN declaration name span (M3 spec §5: querying a
    // declaration must resolve that same function).
    for (const auto& [id, info] : functions_) {
        if (rangeContains(info.definition, position)) {
            return id;
        }
    }
    // A call-site callee span (M3 spec §5: querying `foo(...)` must
    // resolve to the SAME function identity as querying its declaration).
    for (const CallEdge& edge : edges_) {
        if (rangeContains(edge.callSite, position)) {
            return edge.callee;
        }
    }
    // Neither - a Local/Parameter/whitespace/etc. position, or a call
    // that resolved to a Builtin (never indexed as an edge at all - see
    // indexExpr()). M3 spec §6: a valid query with no function target.
    return std::nullopt;
}

std::vector<CallSiteGroup> SemanticCallQuery::groupCalleesOf(SymbolId caller) const {
    // First-call-occurrence order (M3 spec §12): `grouped` accumulates
    // callees in the order their FIRST call site is encountered while
    // scanning `edges_` (already in source/traversal order), and each
    // group's own callSites therefore also end up in source order.
    std::vector<std::pair<SymbolId, std::vector<InspectionRange>>> grouped;
    for (const CallEdge& edge : edges_) {
        if (edge.caller != caller) {
            continue;
        }
        const auto it =
            std::find_if(grouped.begin(), grouped.end(), [&](const auto& group) { return group.first == edge.callee; });
        if (it == grouped.end()) {
            grouped.push_back({edge.callee, std::vector<InspectionRange>{edge.callSite}});
        } else {
            it->second.push_back(edge.callSite);
        }
    }

    std::vector<CallSiteGroup> result;
    result.reserve(grouped.size());
    for (auto& [calleeId, sites] : grouped) {
        const SemanticSymbolInfo* calleeInfo = functionInfoFor(calleeId);
        assert(calleeInfo != nullptr); // every edge's callee was resolved to SymbolKind::Function, so it is indexed
        result.push_back(CallSiteGroup{*calleeInfo, std::move(sites)});
    }
    return result;
}

CallRelationResult SemanticCallQuery::findCallers(InspectionPosition position) const {
    CallRelationResult result;
    const std::optional<SymbolId> targetId = resolveFunctionAt(position);
    if (!targetId.has_value()) {
        return result; // {nullopt, {}}
    }
    result.function = *functionInfoFor(*targetId);

    // CALLER source declaration order (M3 spec §12) - deliberately
    // different from findCallees()'s first-occurrence order: iterate
    // `functions_` (already source-ordered) and keep only those with at
    // least one edge into `targetId`.
    for (const auto& [callerId, callerInfo] : functions_) {
        std::vector<InspectionRange> sites;
        for (const CallEdge& edge : edges_) {
            if (edge.caller == callerId && edge.callee == *targetId) {
                sites.push_back(edge.callSite);
            }
        }
        if (!sites.empty()) {
            result.relations.push_back(CallSiteGroup{callerInfo, std::move(sites)});
        }
    }
    return result;
}

CallRelationResult SemanticCallQuery::findCallees(InspectionPosition position) const {
    CallRelationResult result;
    const std::optional<SymbolId> targetId = resolveFunctionAt(position);
    if (!targetId.has_value()) {
        return result;
    }
    result.function = *functionInfoFor(*targetId);
    result.relations = groupCalleesOf(*targetId);
    return result;
}

CallGraph SemanticCallQuery::directCallGraph() const {
    CallGraph graph;
    graph.functions.reserve(functions_.size());
    for (const auto& [fnId, fnInfo] : functions_) {
        // Every function is a node, even with zero callees (M3 spec §20).
        graph.functions.push_back(CallGraphNode{fnInfo, groupCalleesOf(fnId)});
    }
    return graph;
}

void SemanticCallQuery::indexFile(const ast::SourceFile& file) {
    for (const auto& decl : file.declarations()) {
        switch (decl->kind()) {
            case ast::DeclKind::Function:
                indexFunction(static_cast<const ast::FunctionDecl&>(*decl));
                break;
        }
    }
}

void SemanticCallQuery::indexFunction(const ast::FunctionDecl& fn) {
    const std::optional<SymbolId> fnId = model_.declarationSymbol(fn.name());
    assert(fnId.has_value());
    // M3 spec §30: the shared builder (also used by SemanticInspector/
    // SemanticQuery) - this class needs no per-parameter bookkeeping at
    // all, unlike SemanticQuery, since parameters are never call targets.
    functions_.emplace_back(*fnId, buildFunctionSymbolInfo(sources_, model_, fn));

    indexBlock(fn.body(), *fnId);
}

void SemanticCallQuery::indexBlock(const ast::BlockStmt& block, SymbolId enclosingFunction) {
    for (const auto& stmt : block.statements()) {
        indexStatement(*stmt, enclosingFunction);
    }
}

// No `default:` case: StmtKind is fully implemented today. Every
// statement kind that can contain an expression is walked (M3 spec §16)
// so a call is found regardless of where it appears - VarDecl
// initializers, return values, if/while conditions, and (via
// ExprStmt/nested blocks) ordinary statement expressions. Declarations
// themselves (VarDeclStmt's own name, a `for` loop's own variable) are
// NOT recorded here - this class only ever tracks FUNCTION declarations
// and CALL sites, never Locals/Parameters (M3 spec §8).
void SemanticCallQuery::indexStatement(const ast::Stmt& stmt, SymbolId enclosingFunction) {
    switch (stmt.kind()) {
        case ast::StmtKind::VarDecl:
            indexExpr(static_cast<const ast::VarDeclStmt&>(stmt).initializer(), enclosingFunction);
            return;

        case ast::StmtKind::Block:
            indexBlock(static_cast<const ast::BlockStmt&>(stmt), enclosingFunction);
            return;

        case ast::StmtKind::Expr:
            indexExpr(static_cast<const ast::ExprStmt&>(stmt).expr(), enclosingFunction);
            return;

        case ast::StmtKind::Return: {
            const ast::Expr* value = static_cast<const ast::ReturnStmt&>(stmt).value();
            if (value != nullptr) {
                indexExpr(*value, enclosingFunction);
            }
            return;
        }

        case ast::StmtKind::If: {
            const auto& ifStmt = static_cast<const ast::IfStmt&>(stmt);
            for (const ast::IfBranch& branch : ifStmt.branches()) {
                // Calls inside a condition are still direct STATIC edges
                // (M3 spec §17), regardless of `&&`/`||` short-circuit or
                // which branch runtime execution happens to take.
                indexExpr(*branch.condition, enclosingFunction);
                indexBlock(*branch.body, enclosingFunction);
            }
            if (ifStmt.elseClause().has_value()) {
                indexBlock(*ifStmt.elseClause()->body, enclosingFunction);
            }
            return;
        }

        case ast::StmtKind::While: {
            const auto& whileStmt = static_cast<const ast::WhileStmt&>(stmt);
            indexExpr(whileStmt.condition(), enclosingFunction);
            indexBlock(whileStmt.body(), enclosingFunction);
            return;
        }

        case ast::StmtKind::For: {
            const auto& forStmt = static_cast<const ast::ForStmt&>(stmt);
            indexExpr(forStmt.iterable(), enclosingFunction);
            indexBlock(forStmt.body(), enclosingFunction);
            return;
        }
    }
}

// No `default:` case: ExprKind is fully implemented today. Every current
// expression form is walked purely to REACH nested CallExpr nodes
// (M3 spec §16 - argument, arithmetic, unary, nested-call positions all
// count); `Literal`/`Identifier`/`Unit` carry no calls, and
// `MemberExpr::member()` has no SymbolId behind it yet (no structs), so
// only its `object()` is walked.
void SemanticCallQuery::indexExpr(const ast::Expr& expr, SymbolId enclosingFunction) {
    switch (expr.kind()) {
        case ast::ExprKind::Literal:
        case ast::ExprKind::Identifier:
        case ast::ExprKind::Unit:
            return;

        case ast::ExprKind::Call: {
            const auto& call = static_cast<const ast::CallExpr&>(expr);

            // A direct call edge exists ONLY when the callee resolves
            // (via SemanticModel::resolution() - never identifier text)
            // to SymbolKind::Function. SymbolKind::Builtin (print/panic/
            // assert, when not shadowed) is deliberately excluded (M3
            // spec §3) - no edge is recorded, so a query landing on such
            // a call site simply finds nothing (see resolveFunctionAt()).
            // A user declaration shadowing a builtin (e.g. the user's own
            // `fn print`) resolves to that Function like any other call,
            // with no special-casing here - SemanticModel::resolution()
            // already made that decision during name resolution.
            if (const ast::IdentifierExpr* calleeIdentifier = unwrapDirectCalleeIdentifier(call.callee())) {
                if (const std::optional<SymbolId> calleeId = model_.resolution(*calleeIdentifier)) {
                    const Symbol& calleeSymbol = model_.symbol(*calleeId);
                    if (calleeSymbol.kind == SymbolKind::Function) {
                        edges_.push_back(CallEdge{
                            enclosingFunction,
                            *calleeId,
                            inspectionRangeOf(sources_, calleeIdentifier->span()),
                        });
                    }
                }
            }

            // Still recurse into the callee and every argument - this is
            // what finds a NESTED call such as `outer(inner())` (M3 spec
            // §16): `inner`'s own CallExpr is reached as one of `outer`'s
            // arguments, independently of whatever edge (if any) was
            // just recorded for `outer` itself above.
            indexExpr(call.callee(), enclosingFunction);
            for (const auto& argument : call.arguments()) {
                indexExpr(*argument, enclosingFunction);
            }
            return;
        }

        case ast::ExprKind::Paren:
            indexExpr(static_cast<const ast::ParenExpr&>(expr).inner(), enclosingFunction);
            return;

        case ast::ExprKind::Unary:
            indexExpr(static_cast<const ast::UnaryExpr&>(expr).operand(), enclosingFunction);
            return;

        case ast::ExprKind::Binary: {
            const auto& binary = static_cast<const ast::BinaryExpr&>(expr);
            // Short-circuit `&&`/`||` calls are still direct static
            // edges (M3 spec §17) - both operands are always walked.
            indexExpr(binary.left(), enclosingFunction);
            indexExpr(binary.right(), enclosingFunction);
            return;
        }

        case ast::ExprKind::Assignment: {
            const auto& assignment = static_cast<const ast::AssignmentExpr&>(expr);
            indexExpr(assignment.target(), enclosingFunction);
            indexExpr(assignment.value(), enclosingFunction);
            return;
        }

        case ast::ExprKind::ArrayLiteral:
            for (const auto& element : static_cast<const ast::ArrayLiteralExpr&>(expr).elements()) {
                indexExpr(*element, enclosingFunction);
            }
            return;

        case ast::ExprKind::Index: {
            const auto& index = static_cast<const ast::IndexExpr&>(expr);
            indexExpr(index.object(), enclosingFunction);
            indexExpr(index.index(), enclosingFunction);
            return;
        }

        case ast::ExprKind::Member:
            indexExpr(static_cast<const ast::MemberExpr&>(expr).object(), enclosingFunction);
            return;

        case ast::ExprKind::ErrorPropagation:
            indexExpr(static_cast<const ast::ErrorPropagationExpr&>(expr).operand(), enclosingFunction);
            return;
    }
}

} // namespace kai::semantic
