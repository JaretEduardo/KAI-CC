#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceLocation.hpp"
#include "kai/source/SourceManager.hpp"

#include <optional>
#include <string_view>

namespace kai::semantic {

/// TYPE-CHECK MILESTONE 1: Literal & Annotation Foundation.
///
/// TypeChecker is a SEPARATE pass/class run strictly after
/// SemanticAnalyzer::analyze() has already populated a SemanticModel's
/// Symbol/resolution/declaration state (lexical name resolution). It
/// mutates that same SemanticModel further - recording a semantic Type
/// for every reachable ast::Expr, and updating unannotated Local symbols'
/// inferred Type - but never performs name resolution itself: no scope
/// stack, no Symbol creation, no duplicate/UnknownIdentifier handling.
/// IdentifierExpr typing comes only from model.resolution()/model.symbol()
/// (see checkIdentifierExpr() in TypeChecker.cpp) - a hard architectural
/// boundary.
///
/// This milestone deliberately does NOT implement primitive-operator
/// typing, call validation, assignment semantics, or condition/return
/// validation - every such outer expression kind is still fully
/// traversed (its children are checked) but recorded as
/// Type::unresolved() (see checkBinaryExpr()/checkCallExpr()/etc. in
/// TypeChecker.cpp for the full per-ExprKind rules).
///
/// typeOf() three-state contract (see SemanticModel::typeOf()):
/// std::nullopt means an ast::Expr was never visited by this pass;
/// Type::unresolved() means it WAS visited but this milestone does not
/// model its type (e.g. a deferred outer expression, or a string
/// literal); Type::error() means it WAS visited and a genuine semantic
/// error occurred or was propagated to it (e.g. an out-of-range integer
/// literal, or an identifier whose name never resolved). After check()
/// traverses one SourceFile, every ast::Expr reachable from the function
/// bodies it visits has one of these three states recorded - never
/// silently left without an entry.
class TypeChecker {
public:
    explicit TypeChecker(const SourceManager& sources) noexcept;

    /// Traverses every top-level FunctionDecl's body in `file`, checking
    /// every statement/expression it contains. Assumes
    /// SemanticAnalyzer::analyze(file) has already populated `model`.
    void check(const ast::SourceFile& file, SemanticModel& model);

private:
    // --- Top-level / statement traversal ---

    void checkTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model) const;
    void checkFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model) const;
    void checkBlock(const ast::BlockStmt& block, SemanticModel& model) const;

    /// Exhaustive over ast::StmtKind, no `default:`. No statement
    /// validation happens yet (no Bool-condition requirement, no
    /// return-type comparison, no for-variable element-type inference) -
    /// see this class's own comment above.
    void checkStatement(const ast::Stmt& stmt, SemanticModel& model) const;

    /// Implements the annotated-local and unannotated-local algorithms
    /// (Milestone 1 spec #19/#20): fetches the Local Symbol ALREADY
    /// created by SemanticAnalyzer through declarationSymbol() - never
    /// re-resolves the TypeSyntax annotation itself.
    void checkVarDecl(const ast::VarDeclStmt& varDecl, SemanticModel& model) const;

    // --- Expression checking ---

    /// The one core bidirectional operation (Milestone 1 spec #6):
    /// `expected` supplies contextual typing only when it holds a
    /// concrete Type (not nullopt, not Type::unresolved(), not
    /// Type::error() - see usableContext() in TypeChecker.cpp).
    /// `expectedAnnotationSpan` is not part of the conceptual API - it is
    /// a diagnostic-only extra, threaded exclusively from checkVarDecl()
    /// through ParenExpr/Negate-literal wrappers, so a LiteralOutOfRange
    /// raised against an explicit `let x: T = ...` annotation can point
    /// its relatedSpan at that annotation (Milestone 1 spec #24). Every
    /// other call site omits it.
    Type checkExpr(const ast::Expr& expr, std::optional<Type> expected, SemanticModel& model,
                    std::optional<SourceSpan> expectedAnnotationSpan = std::nullopt) const;

    /// Thin wrapper - contains no separate typing algorithm of its own.
    Type inferExpr(const ast::Expr& expr, SemanticModel& model) const;

    Type checkLiteralExpr(const ast::LiteralExpr& literal, std::optional<Type> expected,
                           std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const;

    Type checkIdentifierExpr(const ast::IdentifierExpr& identifier, SemanticModel& model) const;

    Type checkParenExpr(const ast::ParenExpr& paren, std::optional<Type> expected,
                         std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const;

    /// Special-cases ONLY Negate directly (through transparent ParenExpr
    /// wrappers) over an Integer/Float LiteralExpr, treating the whole
    /// negated-literal subtree as one contextually-typed numeric constant
    /// (Milestone 1 spec #13/#14). Every other UnaryExpr shape - general
    /// `-identifier`, `!expr`, `&expr`, `&mut expr` - stays
    /// Type::unresolved(), though its operand is still fully checked.
    Type checkUnaryExpr(const ast::UnaryExpr& unary, std::optional<Type> expected,
                         std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const;

    // Deferred outer expression kinds (Milestone 1 spec #8/#21): each
    // checks all of its children with no expected context and always
    // records Type::unresolved() for itself, even when a child comes
    // back Type::error() - a child's error is deliberately NOT
    // propagated to these outer nodes in this milestone (see
    // TypeChecker.cpp).
    Type checkBinaryExpr(const ast::BinaryExpr& binary, SemanticModel& model) const;
    Type checkCallExpr(const ast::CallExpr& call, SemanticModel& model) const;
    Type checkAssignmentExpr(const ast::AssignmentExpr& assignment, SemanticModel& model) const;
    Type checkArrayLiteralExpr(const ast::ArrayLiteralExpr& array, SemanticModel& model) const;
    Type checkIndexExpr(const ast::IndexExpr& index, SemanticModel& model) const;
    Type checkMemberExpr(const ast::MemberExpr& member, SemanticModel& model) const;
    Type checkErrorPropagationExpr(const ast::ErrorPropagationExpr& errorPropagation, SemanticModel& model) const;

    /// Decodes `text` (an integer literal's exact source spelling) as an
    /// unsigned magnitude and range-checks it, sign-aware, against
    /// `context` if it names a concrete integer type, or against
    /// Type::i32() otherwise (Milestone 1 spec #11/#12/#13/#27). Records
    /// no SemanticError kind other than LiteralOutOfRange, and records no
    /// expression type of its own - callers do that themselves, since a
    /// negative-literal caller must additionally stamp the same Type onto
    /// the ParenExpr/UnaryExpr wrapping the literal.
    Type checkIntegerLiteralValue(std::string_view text, bool negative, SourceSpan diagnosticSpan,
                                   std::optional<Type> context, std::optional<SourceSpan> expectedAnnotationSpan,
                                   SemanticModel& model) const;

    const SourceManager& sources_;
};

} // namespace kai::semantic
