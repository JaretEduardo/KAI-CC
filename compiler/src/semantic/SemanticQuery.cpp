#include "kai/semantic/SemanticQuery.hpp"

#include <cassert>

namespace kai::semantic {

namespace {

// Half-open containment, matching InspectionRange's own documented
// convention exactly (M2 spec §5): `position` matches `range` iff it
// lies in [start, end). Every occurrence this class indexes is a single
// Identifier's span, which can never cross a line (the lexer only scans
// identifier characters), so the multi-line branches below are dead code
// in practice today - handled anyway rather than assumed away, since
// nothing prevents a future occurrence kind from spanning lines.
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
    return true; // strictly between start.line and end.line
}

} // namespace

SemanticQuery::SemanticQuery(const SourceManager& sources, const SemanticModel& model, const ast::SourceFile& file)
    : sources_(sources), model_(model) {
    indexFile(file);
}

DefinitionResult SemanticQuery::findDefinition(InspectionPosition position) const {
    const Occurrence* occurrence = occurrenceAt(position);
    if (occurrence == nullptr) {
        return std::nullopt;
    }
    const SemanticSymbolInfo* info = symbolInfoFor(occurrence->id);
    if (info == nullptr) {
        // Resolves to a Builtin (or, in principle, some other symbol
        // this milestone never indexes a declaration for) - M2 spec
        // §16: "no source definition" is modeled as the same "no
        // symbol" result, never a fabricated location.
        return std::nullopt;
    }
    return *info;
}

ReferencesResult SemanticQuery::findReferences(InspectionPosition position) const {
    ReferencesResult result;

    const Occurrence* occurrence = occurrenceAt(position);
    if (occurrence == nullptr) {
        return result; // {nullopt, {}}
    }
    const SemanticSymbolInfo* info = symbolInfoFor(occurrence->id);
    if (info == nullptr) {
        return result; // Builtin (or similarly undeclared) occurrence - same "no symbol" policy as findDefinition()
    }
    result.symbol = *info;

    // Source order is already guaranteed here: `occurrences_` is
    // populated by one single, source-ordered AST traversal (indexFile())
    // and never reordered afterward.
    for (const Occurrence& candidate : occurrences_) {
        if (!candidate.isDeclaration && candidate.id == occurrence->id) {
            result.references.push_back(candidate.range);
        }
    }
    return result;
}

const SemanticQuery::Occurrence* SemanticQuery::occurrenceAt(InspectionPosition position) const {
    for (const Occurrence& occurrence : occurrences_) {
        if (rangeContains(occurrence.range, position)) {
            return &occurrence;
        }
    }
    return nullptr;
}

const SemanticSymbolInfo* SemanticQuery::symbolInfoFor(SymbolId id) const {
    for (const auto& [candidateId, info] : declaredSymbols_) {
        if (candidateId == id) {
            return &info;
        }
    }
    return nullptr;
}

void SemanticQuery::addDeclaration(SymbolId id, InspectionRange range, SemanticSymbolInfo info) {
    occurrences_.push_back(Occurrence{id, range, /*isDeclaration=*/true});
    declaredSymbols_.emplace_back(id, std::move(info));
}

void SemanticQuery::addUse(const ast::IdentifierExpr& identifier) {
    // Identity comes only from resolution - never identifier source text
    // (mirrors lowerIdentifierExpr()'s own established rule). A
    // resolution-less identifier cannot occur in a program this class is
    // ever run against (inspect/query only run after a fully successful
    // frontend pass - an unresolved identifier would already be an
    // UnknownIdentifier error), but this is still handled defensively
    // rather than assumed.
    const std::optional<SymbolId> id = model_.resolution(identifier);
    if (!id.has_value()) {
        return;
    }
    occurrences_.push_back(Occurrence{*id, inspectionRangeOf(sources_, identifier.span()), /*isDeclaration=*/false});
}

void SemanticQuery::indexFile(const ast::SourceFile& file) {
    // Same exhaustive-DeclKind-switch shape as SemanticInspector::inspect().
    for (const auto& decl : file.declarations()) {
        switch (decl->kind()) {
            case ast::DeclKind::Function:
                indexFunction(static_cast<const ast::FunctionDecl&>(*decl));
                break;
        }
    }
}

void SemanticQuery::indexFunction(const ast::FunctionDecl& fn) {
    // This declaration-side indexing intentionally mirrors
    // SemanticInspector::collectFunction()'s own construction of a
    // Function/Parameter SemanticSymbolInfo - both classes need the
    // identical "declaration -> tooling symbol info" mapping, just from
    // two different entry points (enumerate-everything vs. index-by-
    // position), so this is the SAME semantic fact computed twice, not
    // two independently-invented shapes.
    const std::optional<SymbolId> fnId = model_.declarationSymbol(fn.name());
    assert(fnId.has_value());
    const Symbol& fnSymbol = model_.symbol(*fnId);
    assert(fnSymbol.declaredAt.has_value());
    assert(fnSymbol.signature.has_value());

    const std::string name(sources_.text(fn.name().span));
    const FunctionSignature& signature = *fnSymbol.signature;
    const InspectionRange fnDefinition = inspectionRangeOf(sources_, *fnSymbol.declaredAt);

    SemanticSymbolInfo functionInfo;
    functionInfo.name = name;
    functionInfo.kind = SemanticSymbolKind::Function;
    functionInfo.definition = fnDefinition;
    functionInfo.returnType = signature.returnType;

    for (const ast::Param& param : fn.params()) {
        const std::optional<SymbolId> paramId = model_.declarationSymbol(param.name);
        assert(paramId.has_value());
        const Symbol& paramSymbol = model_.symbol(*paramId);
        assert(paramSymbol.declaredAt.has_value());
        functionInfo.parameters.push_back(SemanticParameterInfo{
            std::string(sources_.text(param.name.span)),
            paramSymbol.type,
            inspectionRangeOf(sources_, *paramSymbol.declaredAt),
        });
    }

    addDeclaration(*fnId, fnDefinition, functionInfo);

    for (const ast::Param& param : fn.params()) {
        const std::optional<SymbolId> paramId = model_.declarationSymbol(param.name);
        const Symbol& paramSymbol = model_.symbol(*paramId);
        const InspectionRange paramDefinition = inspectionRangeOf(sources_, *paramSymbol.declaredAt);

        SemanticSymbolInfo paramInfo;
        paramInfo.name = std::string(sources_.text(param.name.span));
        paramInfo.kind = SemanticSymbolKind::Parameter;
        paramInfo.definition = paramDefinition;
        paramInfo.type = paramSymbol.type;
        paramInfo.enclosingFunction = name;
        addDeclaration(*paramId, paramDefinition, std::move(paramInfo));
    }

    indexBlock(fn.body(), name);
}

void SemanticQuery::indexBlock(const ast::BlockStmt& block, const std::string& enclosingFunction) {
    for (const auto& stmt : block.statements()) {
        indexStatement(*stmt, enclosingFunction);
    }
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// SemanticInspector::collectStatement()'s own exhaustive switch. Unlike
// SemanticInspector, every statement kind that CAN carry an expression is
// walked here too (M2 spec §8: use occurrences must be found inside
// initializers, conditions, return values, etc., not just declaration
// sites).
void SemanticQuery::indexStatement(const ast::Stmt& stmt, const std::string& enclosingFunction) {
    switch (stmt.kind()) {
        case ast::StmtKind::VarDecl: {
            const auto& varDecl = static_cast<const ast::VarDeclStmt&>(stmt);
            const std::optional<SymbolId> id = model_.declarationSymbol(varDecl.name());
            assert(id.has_value());
            const Symbol& symbol = model_.symbol(*id);
            assert(symbol.declaredAt.has_value());

            const InspectionRange definition = inspectionRangeOf(sources_, *symbol.declaredAt);
            SemanticSymbolInfo local;
            local.name = std::string(sources_.text(varDecl.name().span));
            local.kind = SemanticSymbolKind::Local;
            local.definition = definition;
            local.type = symbol.type;
            local.enclosingFunction = enclosingFunction;
            addDeclaration(*id, definition, std::move(local));

            indexExpr(varDecl.initializer());
            return;
        }

        case ast::StmtKind::Block:
            indexBlock(static_cast<const ast::BlockStmt&>(stmt), enclosingFunction);
            return;

        case ast::StmtKind::Expr:
            indexExpr(static_cast<const ast::ExprStmt&>(stmt).expr());
            return;

        case ast::StmtKind::Return: {
            const ast::Expr* value = static_cast<const ast::ReturnStmt&>(stmt).value();
            if (value != nullptr) {
                indexExpr(*value);
            }
            return;
        }

        case ast::StmtKind::If: {
            const auto& ifStmt = static_cast<const ast::IfStmt&>(stmt);
            for (const ast::IfBranch& branch : ifStmt.branches()) {
                indexExpr(*branch.condition);
                indexBlock(*branch.body, enclosingFunction);
            }
            if (ifStmt.elseClause().has_value()) {
                indexBlock(*ifStmt.elseClause()->body, enclosingFunction);
            }
            return;
        }

        case ast::StmtKind::While: {
            const auto& whileStmt = static_cast<const ast::WhileStmt&>(stmt);
            indexExpr(whileStmt.condition());
            indexBlock(whileStmt.body(), enclosingFunction);
            return;
        }

        case ast::StmtKind::For: {
            const auto& forStmt = static_cast<const ast::ForStmt&>(stmt);
            indexExpr(forStmt.iterable());

            const std::optional<SymbolId> id = model_.declarationSymbol(forStmt.variable());
            assert(id.has_value());
            const Symbol& symbol = model_.symbol(*id);
            assert(symbol.declaredAt.has_value());

            const InspectionRange definition = inspectionRangeOf(sources_, *symbol.declaredAt);
            SemanticSymbolInfo local;
            local.name = std::string(sources_.text(forStmt.variable().span));
            local.kind = SemanticSymbolKind::Local;
            local.definition = definition;
            local.type = symbol.type;
            local.enclosingFunction = enclosingFunction;
            addDeclaration(*id, definition, std::move(local));

            indexBlock(forStmt.body(), enclosingFunction);
            return;
        }
    }
}

// No `default:` case: ExprKind is fully implemented today. Every
// CURRENT identifier-bearing expression form is walked (M2 spec §8) -
// this is a frontend semantic-query traversal, deliberately independent
// of what LLVM codegen happens to lower. `Literal`/`Unit` carry no
// identifiers; `MemberExpr::member()` is a plain (non-resolvable)
// Identifier today - member/field resolution does not exist yet (no
// structs), so there is no SymbolId behind it to index - only its
// `object()` sub-expression is walked.
void SemanticQuery::indexExpr(const ast::Expr& expr) {
    switch (expr.kind()) {
        case ast::ExprKind::Literal:
        case ast::ExprKind::Unit:
            return;

        case ast::ExprKind::Identifier:
            addUse(static_cast<const ast::IdentifierExpr&>(expr));
            return;

        case ast::ExprKind::Call: {
            const auto& call = static_cast<const ast::CallExpr&>(expr);
            indexExpr(call.callee());
            for (const auto& argument : call.arguments()) {
                indexExpr(*argument);
            }
            return;
        }

        case ast::ExprKind::Paren:
            indexExpr(static_cast<const ast::ParenExpr&>(expr).inner());
            return;

        case ast::ExprKind::Unary:
            indexExpr(static_cast<const ast::UnaryExpr&>(expr).operand());
            return;

        case ast::ExprKind::Binary: {
            const auto& binary = static_cast<const ast::BinaryExpr&>(expr);
            indexExpr(binary.left());
            indexExpr(binary.right());
            return;
        }

        case ast::ExprKind::Assignment: {
            const auto& assignment = static_cast<const ast::AssignmentExpr&>(expr);
            // The target is walked through the SAME generic expression
            // path as any other operand - an IdentifierExpr target is
            // therefore indexed as an ordinary use (M2 spec §12: "an
            // assignment target counts as a reference"), with no
            // special-cased "assignment target" logic needed here.
            indexExpr(assignment.target());
            indexExpr(assignment.value());
            return;
        }

        case ast::ExprKind::ArrayLiteral:
            for (const auto& element : static_cast<const ast::ArrayLiteralExpr&>(expr).elements()) {
                indexExpr(*element);
            }
            return;

        case ast::ExprKind::Index: {
            const auto& index = static_cast<const ast::IndexExpr&>(expr);
            indexExpr(index.object());
            indexExpr(index.index());
            return;
        }

        case ast::ExprKind::Member:
            indexExpr(static_cast<const ast::MemberExpr&>(expr).object());
            return;

        case ast::ExprKind::ErrorPropagation:
            indexExpr(static_cast<const ast::ErrorPropagationExpr&>(expr).operand());
            return;
    }
}

} // namespace kai::semantic
