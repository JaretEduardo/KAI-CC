#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"

namespace kai::semantic {

/// CONTROL-FLOW MILESTONE 1: Return Completeness / Structural All-Paths-
/// Return.
///
/// ControlFlowAnalyzer is a separate pass run strictly after
/// TypeChecker::check() - it is the third stage of the semantic pipeline
/// (SemanticAnalyzer -> TypeChecker -> ControlFlowAnalyzer), appending
/// further SemanticError diagnostics to the same SemanticModel. Unlike
/// the two passes before it, it answers a purely structural question -
/// "can control reach the end of this statement/block" - never a typing
/// question: it never calls checkExpr, never reads
/// SemanticModel::typeOf(), never resolves a TypeSyntax, and never
/// performs name resolution. Its only semantic Type dependency is
/// FunctionSignature.returnType, read once per function purely to decide
/// whether return completeness is even required.
///
/// This milestone deliberately stays structural rather than building
/// general CFG infrastructure: it does not evaluate constant conditions,
/// does not model divergence/Never, does not know about break/continue
/// (KAI 0.1 has neither yet), and does not diagnose unreachable code -
/// every one of those is separate, later work. `while`/`for` are
/// unconditionally treated as possibly not executing at all (see
/// analyzeStatement() in ControlFlowAnalyzer.cpp) - a deliberate,
/// documented conservative limitation, not an oversight.
///
/// Stateless and reentrant: holds no member state at all, so one
/// ControlFlowAnalyzer instance (or a fresh one per call) may safely
/// analyze multiple, independent SourceFiles in any order.
class ControlFlowAnalyzer {
public:
    ControlFlowAnalyzer() = default;

    /// Traverses every top-level FunctionDecl in `file`, checking return
    /// completeness for each. Assumes SemanticAnalyzer::analyze(file) has
    /// already populated `model` with Symbol/FunctionSignature state.
    /// TypeChecker::check(file, model) is expected to have already run
    /// too (this is the third pipeline stage), though this pass does not
    /// actually depend on TypeChecker's own results - see the class
    /// comment above.
    void check(const ast::SourceFile& file, SemanticModel& model) const;

private:
    /// Whether control can fall off the end of a statement/block
    /// (FallsThrough) or is guaranteed to reach a ReturnStmt on every
    /// path through it (AlwaysReturns). A purely structural fact about
    /// AST shape - never derived from, or mixed with, any expression
    /// Type. Deliberately just these two states in this milestone: no
    /// Diverges/Unreachable/Breaks/Continues.
    enum class FlowResult {
        FallsThrough,
        AlwaysReturns,
    };

    void checkTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model) const;

    /// Skips completeness entirely for a Unit, Error, or Unresolved
    /// declared return Type; for a concrete non-Unit return Type,
    /// analyzes the body and emits exactly one MissingReturn if it can
    /// fall through. Never re-resolves TypeSyntax and never mutates
    /// FunctionSignature - the declared return Type is read once, by
    /// value, from the Symbol SemanticAnalyzer already created.
    void checkFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model) const;

    FlowResult analyzeBlock(const ast::BlockStmt& block) const;

    /// Exhaustive over ast::StmtKind, no `default:`. ReturnStmt is always
    /// AlwaysReturns regardless of its own value's type-correctness;
    /// ExprStmt/VarDeclStmt/While/For are always FallsThrough, with no
    /// inspection of their expressions' semantic Types; a nested
    /// BlockStmt delegates to analyzeBlock(); If delegates to
    /// analyzeIfStmt().
    FlowResult analyzeStatement(const ast::Stmt& stmt) const;

    /// AlwaysReturns iff a final `else` clause exists AND every branch
    /// body (the initial `if`, every `else if`, and the final `else`) is
    /// itself AlwaysReturns. The condition expressions themselves are
    /// never inspected - no constant-condition reasoning exists in this
    /// milestone, so `if true { return 1 }` with no `else` is still
    /// FallsThrough.
    FlowResult analyzeIfStmt(const ast::IfStmt& ifStmt) const;
};

} // namespace kai::semantic
