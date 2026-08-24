#include "kai/semantic/ControlFlowAnalyzer.hpp"

#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/Symbol.hpp"

#include <cassert>
#include <optional>

namespace kai::semantic {

void ControlFlowAnalyzer::check(const ast::SourceFile& file, SemanticModel& model) const {
    for (const auto& decl : file.declarations()) {
        checkTopLevelDeclaration(*decl, model);
    }
}

// No `default:` case: DeclKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's/TypeChecker.cpp's own exhaustive switch over it.
void ControlFlowAnalyzer::checkTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model) const {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            checkFunctionDecl(static_cast<const ast::FunctionDecl&>(decl), model);
            return;
    }
}

void ControlFlowAnalyzer::checkFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model) const {
    // Declaration mapping, not a name lookup - mirrors TypeChecker's own
    // established pattern.
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    // Copied by value - never re-resolved, never mutated.
    const Type returnType = model.symbol(*fnId).signature->returnType;

    if (returnType.isError() || returnType.isUnresolved() || returnType == Type::unit()) {
        // Unit: no completeness requirement. Error/Unresolved: the
        // declaration's own return annotation already failed or is
        // deferred - skip rather than cascade a second diagnostic onto
        // an already-known problem, or onto a shape this milestone
        // intentionally does not model yet.
        return;
    }

    if (analyzeBlock(fn.body()) == FlowResult::FallsThrough) {
        // A concrete, non-Unit declared return Type is only ever produced
        // from an EXPLICIT `-> T` annotation - SemanticAnalyzer's
        // resolveFunctionSignature() resolves an implicit return
        // unconditionally to Type::unit() - so fn.returnType() is
        // guaranteed non-null here.
        assert(fn.returnType() != nullptr);
        model.addError(SemanticError{
            SemanticErrorKind::MissingReturn,
            fn.returnType()->span(),
            std::nullopt,
            returnType,
            std::nullopt,
        });
    }
}

ControlFlowAnalyzer::FlowResult ControlFlowAnalyzer::analyzeBlock(const ast::BlockStmt& block) const {
    for (const auto& stmt : block.statements()) {
        if (analyzeStatement(*stmt) == FlowResult::AlwaysReturns) {
            // Stops FLOW COMPUTATION only - TypeChecker already traversed
            // every statement, including any after this one, in its own,
            // earlier, separate pass. No unreachable-code diagnostic is
            // produced here, and no prior diagnostic is ever suppressed
            // by this early return.
            return FlowResult::AlwaysReturns;
        }
    }
    return FlowResult::FallsThrough;
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's/TypeChecker.cpp's own exhaustive switch over it.
ControlFlowAnalyzer::FlowResult ControlFlowAnalyzer::analyzeStatement(const ast::Stmt& stmt) const {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            return analyzeBlock(static_cast<const ast::BlockStmt&>(stmt));

        case ast::StmtKind::Expr:
        case ast::StmtKind::VarDecl:
            // Ordinary statements never themselves terminate control
            // flow - their expressions' semantic Types are never
            // inspected here.
            return FlowResult::FallsThrough;

        case ast::StmtKind::Return:
            // Unconditional, regardless of the returned value's own
            // type-correctness - a malformed return still structurally
            // terminates this path.
            return FlowResult::AlwaysReturns;

        case ast::StmtKind::If:
            return analyzeIfStmt(static_cast<const ast::IfStmt&>(stmt));

        case ast::StmtKind::While:
        case ast::StmtKind::For:
            // Conservatively FallsThrough, unconditionally, regardless of
            // body - the condition/iterable is never inspected, and the
            // body's own flow result is irrelevant to this milestone's
            // result for the loop itself: a `for` loop may execute zero
            // times, and a `while` loop cannot be soundly proven to
            // execute without constant-condition reasoning, which this
            // milestone deliberately omits. A documented, accepted
            // limitation, not an oversight.
            return FlowResult::FallsThrough;
    }

    // Unreachable while StmtKind's enumerators match the switch above
    // exactly - kept only so -Wreturn-type doesn't warn; the switch
    // itself still has no `default:`.
    return FlowResult::FallsThrough;
}

ControlFlowAnalyzer::FlowResult ControlFlowAnalyzer::analyzeIfStmt(const ast::IfStmt& ifStmt) const {
    // No final `else` - control can always fall out the bottom,
    // regardless of what every branch body does. The condition
    // expressions themselves are never inspected anywhere in this
    // function - no constant-condition reasoning exists in this
    // milestone.
    if (!ifStmt.elseClause().has_value()) {
        return FlowResult::FallsThrough;
    }

    for (const ast::IfBranch& branch : ifStmt.branches()) {
        if (analyzeBlock(*branch.body) == FlowResult::FallsThrough) {
            return FlowResult::FallsThrough;
        }
    }

    if (analyzeBlock(*ifStmt.elseClause()->body) == FlowResult::FallsThrough) {
        return FlowResult::FallsThrough;
    }

    return FlowResult::AlwaysReturns;
}

} // namespace kai::semantic
