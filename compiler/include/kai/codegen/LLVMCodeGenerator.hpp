#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <memory>
#include <optional>

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 1+2: Minimal LLVM Module + Function + Integer
/// Return (M1), Primitive Expression Lowering (M2).
///
/// LLVMCodeGenerator lowers an already-fully-checked AST into an
/// llvm::Module:
///
///     AST + SemanticModel -> LLVM IR
///
/// with no HIR stage (a deliberate, MVP-deadline-driven decision - HIR
/// remains post-MVP work). It performs NO semantic work of its own: no
/// name resolution, no TypeSyntax re-resolution, no type checking, no
/// type inference, and it never mutates SemanticModel or the AST.
/// SemanticModel::declarationSymbol()/symbol()/typeOf() are read-only
/// semantic ground truth here, exactly as SemanticAnalyzer/TypeChecker/
/// ControlFlowAnalyzer already left them - the same three-pass frontend
/// contract, with this as a fourth, final, non-mutating consumer.
///
/// Callers must run SemanticAnalyzer::analyze(), TypeChecker::check(),
/// and ControlFlowAnalyzer::check() on `file`/`model` before calling
/// generate() - this class assumes the frontend already succeeded
/// (SemanticModel::errors() is empty) and does not re-validate that.
///
/// M1+M2 narrowly support exactly one function shape:
///
///     fn name() -> <primitive scalar type> {
///         return <expression>
///     }
///
/// - zero parameters only (FunctionSignature::parameterTypes must be
///   empty) - parameter lowering is explicitly out of scope, not merely
///   unimplemented by oversight.
/// - a body consisting of exactly one statement: a ReturnStmt.
/// - whose value is a VALUE-PRODUCING primitive expression: an integer,
///   float, or bool literal; a parenthesized expression; a unary Negate/
///   Not; or a binary arithmetic/comparison/equality/logical operator -
///   see lowerExpr() in LLVMCodeGenerator.cpp for the exact per-ExprKind
///   rules and LLVM opcode/predicate selection.
///
/// Any other function shape, statement kind, or expression kind causes
/// generate() to fail explicitly (return false) rather than emit partial
/// or malformed LLVM IR - see generate()'s own documentation. Locals
/// (`let`/`mut`, identifier loads), assignment, function parameters,
/// calls, control flow (if/while/for), Range, Unit-valued expressions,
/// and every non-primitive-scalar semantic Type (references, arrays,
/// str/String, structs, enums, generics, Result, Option, ...) are
/// explicitly deferred to later LLVM codegen milestones (M3+).
///
/// `&&`/`||` lower as eager (non-short-circuiting) LLVM `and`/`or` on i1
/// operands - see lowerBinaryExpr()'s own comment for why this is safe
/// for exactly the pure, side-effect-free expression grammar M2 lowers,
/// and why it must be revisited before a later milestone introduces a
/// side-effecting expression (a call) into logical operand position.
class LLVMCodeGenerator {
public:
    /// `sources` must outlive every generate() call - it is the only way
    /// to recover a literal's source text (LiteralExpr never stores a
    /// decoded value; see LiteralExpr's own class comment). This mirrors
    /// TypeChecker's existing, identical convention and reuses the same
    /// decode-from-text technique TypeChecker.cpp already uses for
    /// integer literal range checking, rather than inventing a second
    /// literal-decoding algorithm in codegen - see lowerLiteralExpr() in
    /// LLVMCodeGenerator.cpp for the exact distinction between that
    /// (purely mechanical, decimal-text-to-number) decoding and the
    /// semantic inference/range-policy work TypeChecker alone owns.
    explicit LLVMCodeGenerator(const SourceManager& sources);

    /// Lowers every top-level FunctionDecl in `file` into a fresh
    /// llvm::Module (replacing any module owned from a previous
    /// generate() call), then runs llvm::verifyModule() on the result.
    /// Returns true only if the entire file lowered successfully AND the
    /// module verified; on false, the previous module() is discarded
    /// (reset to empty) so a caller can never read a partially-generated
    /// or unverified module by calling module() after a failed
    /// generate() - see module()'s precondition below.
    bool generate(const ast::SourceFile& file, const semantic::SemanticModel& model);

    /// The module produced by the most recent successful generate() call.
    ///
    /// Precondition: the most recent generate() call returned true.
    const llvm::Module& module() const;

private:
    bool generateTopLevelDecl(const ast::Decl& decl, const semantic::SemanticModel& model);
    bool generateFunction(const ast::FunctionDecl& fn, const semantic::SemanticModel& model);
    bool generateReturnStmt(const ast::ReturnStmt& stmt, llvm::BasicBlock& block,
                             const semantic::SemanticModel& model);

    /// The central expression dispatcher (M2's main architectural
    /// addition): lowers any value-producing expression `lowerExpr()`
    /// currently supports into one LLVM SSA value, or std::nullopt on
    /// failure. std::nullopt is used - rather than a nullable
    /// `llvm::Value*` - specifically so a genuine future Unit-valued
    /// expression (M3+) can be represented unambiguously alongside
    /// generation failure; M2 itself never produces a "no value"
    /// success case, but the return type already leaves room for one
    /// without an ambiguous nullptr overload. Reads
    /// `model.typeOf(expr)` as semantic ground truth for every dispatch
    /// decision - never re-infers a type independently. Fails (returns
    /// std::nullopt) for an Error/Unresolved semantic Type, or for any
    /// ExprKind this milestone does not lower (Identifier, Call,
    /// Assignment, ArrayLiteral, Index, Member, Unit, ErrorPropagation).
    std::optional<llvm::Value*> lowerExpr(const ast::Expr& expr, const semantic::SemanticModel& model,
                                           llvm::IRBuilder<>& builder);

    std::optional<llvm::Value*> lowerLiteralExpr(const ast::LiteralExpr& literal, semantic::Type type,
                                                  llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerUnaryExpr(const ast::UnaryExpr& unary, const semantic::SemanticModel& model,
                                                llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerBinaryExpr(const ast::BinaryExpr& binary, const semantic::SemanticModel& model,
                                                 llvm::IRBuilder<>& builder);

    /// Maps a semantic::Type to its LLVM counterpart, covering every
    /// primitive scalar kind this milestone's Type models (Unit and the
    /// integer/float/bool kinds) - a small, already-trivial table.
    /// Returns nullptr for Unresolved, Error, and Char - never a
    /// fabricated placeholder LLVM type - so an unsupported semantic
    /// Type causes an explicit generation failure at the call site
    /// instead.
    llvm::Type* lowerType(semantic::Type type);

    const SourceManager& sources_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
};

} // namespace kai::codegen
