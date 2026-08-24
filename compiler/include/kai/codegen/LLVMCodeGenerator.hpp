#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <memory>

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 1: Minimal LLVM Module + Function + Integer
/// Return.
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
/// M1 narrowly supports exactly one function shape:
///
///     fn name() -> <integer type> {
///         return <integer-literal>
///     }
///
/// - zero parameters only (FunctionSignature::parameterTypes must be
///   empty) - parameter lowering is explicitly out of scope, not merely
///   unimplemented by oversight.
/// - a body consisting of exactly one statement: a ReturnStmt.
/// - whose value is a plain (non-negated) integer literal, typed by
///   TypeChecker as one of KAI's integer types.
///
/// Any other function shape, statement kind, or return-value expression
/// causes generate() to fail explicitly (return false) rather than emit
/// partial or malformed LLVM IR - see generate()'s own documentation.
/// Locals, operators, calls, control flow, parameters, and every
/// non-integer semantic Type (references, arrays, str/String, structs,
/// enums, generics, Result, Option, ...) are explicitly deferred to
/// later LLVM codegen milestones (M2+).
class LLVMCodeGenerator {
public:
    /// `sources` must outlive every generate() call - it is the only way
    /// to recover an integer literal's source text (LiteralExpr never
    /// stores a decoded value; see LiteralExpr's own class comment). This
    /// mirrors TypeChecker's existing, identical convention and reuses
    /// the same decode-from-text technique TypeChecker.cpp already uses
    /// for integer literal range checking, rather than inventing a
    /// second literal-decoding algorithm in codegen.
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

    /// Maps a semantic::Type to its LLVM counterpart, covering every
    /// primitive scalar kind this milestone's Type models (Unit and the
    /// integer/float/bool kinds) - a small, already-trivial table, not a
    /// sign that M1 actually exercises all of it (only integer return
    /// types are exercised by generateReturnStmt() today). Returns
    /// nullptr for Unresolved, Error, and Char - never a fabricated
    /// placeholder LLVM type - so an unsupported semantic Type causes an
    /// explicit generation failure at the call site instead.
    llvm::Type* lowerType(semantic::Type type);

    const SourceManager& sources_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
};

} // namespace kai::codegen
