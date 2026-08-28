#include "kai/semantic/SemanticInspector.hpp"

#include <cassert>

namespace kai::semantic {

// M2 spec §7: the SourceSpan -> InspectionRange conversion is now a
// shared free function (declared in SemanticInspector.hpp) so
// SemanticQuery.cpp reuses this exact logic rather than re-deriving it.
InspectionRange inspectionRangeOf(const SourceManager& sources, SourceSpan span) {
    const SourceManager::LineColumn start = sources.lineColumn(span.begin());
    const SourceManager::LineColumn end = sources.lineColumn(span.end());
    return InspectionRange{
        InspectionPosition{start.line, start.column},
        InspectionPosition{end.line, end.column},
    };
}

// M3 spec §30: the shared "declaration -> tooling symbol info" builder
// for a FunctionDecl, now reused by SemanticInspector (below),
// SemanticQuery, and SemanticCallQuery - eliminating what M2 had flagged
// as a 2-way (soon 3-way) duplication.
SemanticSymbolInfo buildFunctionSymbolInfo(const SourceManager& sources, const SemanticModel& model,
                                            const ast::FunctionDecl& fn) {
    // Declaration mapping, not a name lookup - mirrors every other
    // compiler-internal consumer of SemanticModel (TypeChecker,
    // ControlFlowAnalyzer, LLVMCodeGenerator all use this exact pattern).
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());
    const Symbol& fnSymbol = model.symbol(*fnId);
    assert(fnSymbol.declaredAt.has_value()); // every Function Symbol is declared from real source (never a Builtin)
    assert(fnSymbol.signature.has_value());  // Pass 1 of SemanticAnalyzer always resolves one

    SemanticSymbolInfo info;
    info.name = std::string(sources.text(fn.name().span));
    info.kind = SemanticSymbolKind::Function;
    info.definition = inspectionRangeOf(sources, *fnSymbol.declaredAt);
    info.returnType = fnSymbol.signature->returnType;

    info.parameters.reserve(fn.params().size());
    for (const ast::Param& param : fn.params()) {
        const std::optional<SymbolId> paramId = model.declarationSymbol(param.name);
        assert(paramId.has_value());
        const Symbol& paramSymbol = model.symbol(*paramId);
        assert(paramSymbol.declaredAt.has_value());
        info.parameters.push_back(SemanticParameterInfo{
            std::string(sources.text(param.name.span)),
            paramSymbol.type,
            inspectionRangeOf(sources, *paramSymbol.declaredAt),
        });
    }
    return info;
}

SemanticInspectionResult SemanticInspector::inspect(const ast::SourceFile& file) const {
    SemanticInspectionResult result;
    result.file = std::string(sources_.fileName(file.file()));

    // Structural traversal source: the AST, in source order. Only
    // DeclKind::Function exists today (DeclKind's own comment: "extend
    // as later parser milestones add StructDecl/EnumDecl/UseDecl") -
    // this is already an exhaustive switch over the current vocabulary,
    // mirroring every other exhaustive-DeclKind-switch in this codebase.
    for (const auto& decl : file.declarations()) {
        switch (decl->kind()) {
            case ast::DeclKind::Function:
                collectFunction(static_cast<const ast::FunctionDecl&>(*decl), result);
                break;
        }
    }

    return result;
}

void SemanticInspector::collectFunction(const ast::FunctionDecl& fn, SemanticInspectionResult& result) const {
    SemanticSymbolInfo functionInfo = buildFunctionSymbolInfo(sources_, model_, fn);
    const std::string name = functionInfo.name;

    // Flat top-level parameter entries (M1 spec §12: "top-level symbols
    // contains all user-authored symbols") - built FROM the shared
    // helper's own parameter summary above, never re-derived from the
    // AST/SemanticModel a second time.
    std::vector<SemanticSymbolInfo> flatParameters;
    flatParameters.reserve(functionInfo.parameters.size());
    for (const SemanticParameterInfo& param : functionInfo.parameters) {
        SemanticSymbolInfo flatParameter;
        flatParameter.name = param.name;
        flatParameter.kind = SemanticSymbolKind::Parameter;
        flatParameter.definition = param.definition;
        flatParameter.type = param.type;
        flatParameter.enclosingFunction = name;
        flatParameters.push_back(std::move(flatParameter));
    }

    // Function entry first, then its parameters, then (via
    // collectBlock() below) its body's locals - this is the "source
    // declaration order" this milestone documents (M1 spec §21).
    result.symbols.push_back(std::move(functionInfo));
    for (SemanticSymbolInfo& flatParameter : flatParameters) {
        result.symbols.push_back(std::move(flatParameter));
    }

    collectBlock(fn.body(), name, result);
}

void SemanticInspector::collectBlock(const ast::BlockStmt& block, const std::string& enclosingFunction,
                                      SemanticInspectionResult& result) const {
    for (const auto& stmt : block.statements()) {
        collectStatement(*stmt, enclosingFunction, result);
    }
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// LLVMCodeGenerator.cpp's own exhaustive switch over it. Only VarDeclStmt
// and ForStmt (its own loop variable) ever introduce a new Local symbol;
// every other statement kind is traversed purely to keep reaching nested
// blocks (If/While) - Expr/Return never declare anything, and no
// expression can itself contain a VarDeclStmt in this grammar (only a
// BlockStmt's own statement list can), so expressions are never
// separately walked here.
void SemanticInspector::collectStatement(const ast::Stmt& stmt, const std::string& enclosingFunction,
                                          SemanticInspectionResult& result) const {
    switch (stmt.kind()) {
        case ast::StmtKind::VarDecl: {
            const auto& varDecl = static_cast<const ast::VarDeclStmt&>(stmt);
            const std::optional<SymbolId> id = model_.declarationSymbol(varDecl.name());
            assert(id.has_value());
            const Symbol& symbol = model_.symbol(*id);
            assert(symbol.declaredAt.has_value());

            SemanticSymbolInfo local;
            local.name = std::string(sources_.text(varDecl.name().span));
            local.kind = SemanticSymbolKind::Local;
            local.definition = inspectionRangeOf(sources_, *symbol.declaredAt);
            local.type = symbol.type; // TypeChecker's own already-inferred-or-annotated type - never re-inferred here
            local.enclosingFunction = enclosingFunction;
            result.symbols.push_back(std::move(local));
            return;
        }

        case ast::StmtKind::Block:
            collectBlock(static_cast<const ast::BlockStmt&>(stmt), enclosingFunction, result);
            return;

        case ast::StmtKind::If: {
            const auto& ifStmt = static_cast<const ast::IfStmt&>(stmt);
            for (const ast::IfBranch& branch : ifStmt.branches()) {
                collectBlock(*branch.body, enclosingFunction, result);
            }
            if (ifStmt.elseClause().has_value()) {
                collectBlock(*ifStmt.elseClause()->body, enclosingFunction, result);
            }
            return;
        }

        case ast::StmtKind::While:
            collectBlock(static_cast<const ast::WhileStmt&>(stmt).body(), enclosingFunction, result);
            return;

        case ast::StmtKind::For: {
            // The loop variable is a genuine SymbolKind::Local (see
            // SemanticAnalyzer.cpp's analyzeForStmt()) - exposed like any
            // other local. KAI LANGUAGE M6: for a supported integer-range
            // `for i in start..end`, this is now the real matched
            // endpoint element type (e.g. "i32"), pushed by TypeChecker's
            // checkForStmt()/checkIntegerRangeFor(); it stays
            // "unresolved"/"error" for a still-unsupported iterable
            // shape or a mismatched/non-integer range, serialized
            // honestly either way rather than hidden.
            const auto& forStmt = static_cast<const ast::ForStmt&>(stmt);
            const std::optional<SymbolId> id = model_.declarationSymbol(forStmt.variable());
            assert(id.has_value());
            const Symbol& symbol = model_.symbol(*id);
            assert(symbol.declaredAt.has_value());

            SemanticSymbolInfo local;
            local.name = std::string(sources_.text(forStmt.variable().span));
            local.kind = SemanticSymbolKind::Local;
            local.definition = inspectionRangeOf(sources_, *symbol.declaredAt);
            local.type = symbol.type;
            local.enclosingFunction = enclosingFunction;
            result.symbols.push_back(std::move(local));

            collectBlock(forStmt.body(), enclosingFunction, result);
            return;
        }

        case ast::StmtKind::Expr:
        case ast::StmtKind::Return:
            return;
    }
}

} // namespace kai::semantic
