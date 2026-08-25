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

/// SEMANTIC INSPECTION MILESTONE 2: the result of a definition query -
/// the resolved symbol (the SAME SemanticSymbolInfo shape M1's
/// SemanticInspector produces - see this milestone's own §25: never a
/// second, subtly different symbol representation), or std::nullopt if
/// no symbol occurrence exists at the queried position (M2 spec §17:
/// "no symbol here" is a valid, successful query result, never an
/// error).
using DefinitionResult = std::optional<SemanticSymbolInfo>;

/// The result of a references query: the same resolved-symbol-or-null as
/// DefinitionResult, plus every USE occurrence's range (the declaration
/// occurrence itself is never included - M2 spec §9), in SOURCE ORDER.
/// `references` is always empty when `symbol` is std::nullopt.
struct ReferencesResult {
    std::optional<SemanticSymbolInfo> symbol;
    std::vector<InspectionRange> references;
};

/// A reusable, position-based semantic query layer over one already-
/// fully-checked SourceFile - the M2 counterpart to M1's
/// SemanticInspector, built on the exact same architecture: the AST is
/// the STRUCTURAL occurrence source (both declaration identifiers AND
/// every identifier USE this milestone's traversal reaches - see
/// SemanticQuery.cpp's own traversal note for the exact expression/
/// statement coverage), `model` is the SEMANTIC TRUTH for what each
/// occurrence resolves to (`SemanticModel::declarationSymbol()` for a
/// declaration site, `SemanticModel::resolution()` for a use site -
/// never resolution by identifier text).
///
/// Public identity is POSITION, never semantic::SymbolId (M2 spec §2):
/// a caller names an occurrence by (line, column) - required precisely
/// because KAI permits lexical shadowing, so a bare name like `x` is
/// ambiguous but a specific source occurrence never is. SymbolId is used
/// internally, during index construction and lookup, and is NEVER
/// returned to a caller in any form - the same discipline
/// SemanticInspector already established.
///
/// Precondition: same as SemanticInspector - `model` already reflects a
/// successful SemanticAnalyzer + TypeChecker + ControlFlowAnalyzer run
/// over `file`. This class performs no semantic validation of its own.
///
/// Never touches argv, stdout, stderr, or JSON text - a pure, reusable
/// compiler-layer query (see kai::cli::runSemanticQueryCommand for the
/// CLI/JSON consumer).
class SemanticQuery {
public:
    SemanticQuery(const SourceManager& sources, const SemanticModel& model, const ast::SourceFile& file);

    /// The declaration a position resolves to, or std::nullopt if the
    /// position lies inside no known symbol occurrence (declaration OR
    /// use - see M2 spec §4: querying AT a declaration resolves to
    /// itself). A position resolving to a Builtin (e.g. `print`, when
    /// not shadowed) also returns std::nullopt - a Builtin has no source
    /// declaration to report (M2 spec §16: "no source definition" is
    /// modeled as the same "no symbol" result, never a fabricated
    /// location).
    DefinitionResult findDefinition(InspectionPosition position) const;

    /// The same resolved symbol as findDefinition(), plus every OTHER
    /// occurrence (use sites only, never the declaration itself) sharing
    /// that exact internal identity, in source order.
    ReferencesResult findReferences(InspectionPosition position) const;

private:
    /// One indexed occurrence - internal only, NEVER exposed to a
    /// caller. `id` is the only place a semantic::SymbolId appears
    /// anywhere in this class's data.
    struct Occurrence {
        SymbolId id;
        InspectionRange range;
        bool isDeclaration;
    };

    void indexFile(const ast::SourceFile& file);
    void indexFunction(const ast::FunctionDecl& fn);
    void indexBlock(const ast::BlockStmt& block, const std::string& enclosingFunction);
    void indexStatement(const ast::Stmt& stmt, const std::string& enclosingFunction);
    void indexExpr(const ast::Expr& expr);

    void addDeclaration(SymbolId id, InspectionRange range, SemanticSymbolInfo info);
    void addUse(const ast::IdentifierExpr& identifier);

    const Occurrence* occurrenceAt(InspectionPosition position) const;
    const SemanticSymbolInfo* symbolInfoFor(SymbolId id) const;

    const SourceManager& sources_;
    const SemanticModel& model_;

    std::vector<Occurrence> occurrences_;
    /// Linear-scan-by-SymbolId, same rationale as every other SymbolId-
    /// keyed table in this compiler (LLVMCodeGenerator's `locals_`/
    /// `functions_`): SymbolId exposes no public hash/ordering, and a
    /// handful of declarations per file makes a linear scan the smallest
    /// useful representation, not a performance concern.
    std::vector<std::pair<SymbolId, SemanticSymbolInfo>> declaredSymbols_;
};

} // namespace kai::semantic
