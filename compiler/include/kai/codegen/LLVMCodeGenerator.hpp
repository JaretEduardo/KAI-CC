#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Symbol.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 1+2+3+4: Minimal LLVM Module + Function +
/// Integer Return (M1), Primitive Expression Lowering (M2), Local
/// Variables + Identifier Loads + Assignment (M3), Parameters + Function
/// Calls + Recursion + FINAL short-circuit &&/|| (M4).
///
/// LLVMCodeGenerator lowers an already-fully-checked AST into an
/// llvm::Module:
///
///     AST + SemanticModel -> LLVM IR
///
/// with no HIR stage (a deliberate, MVP-deadline-driven decision - HIR
/// remains post-MVP work). It performs NO semantic work of its own: no
/// name resolution, no TypeSyntax re-resolution, no type checking, no
/// type inference, no mutability/assignment-target/arity validation, and
/// it never mutates SemanticModel or the AST. SemanticModel's
/// declaration/resolution mappings and Symbol/FunctionSignature Types are
/// read-only semantic ground truth here, exactly as SemanticAnalyzer/
/// TypeChecker/ControlFlowAnalyzer already left them - the same frontend
/// contract, with this as a final, non-mutating consumer.
///
/// Callers must run SemanticAnalyzer::analyze(), TypeChecker::check(),
/// and ControlFlowAnalyzer::check() on `file`/`model` before calling
/// generate() - this class assumes the frontend already succeeded
/// (SemanticModel::errors() is empty) and does not re-validate that.
///
/// generate() runs a TWO-PASS lowering over `file`'s FunctionDecls (see
/// declareFunction()/defineFunction() in LLVMCodeGenerator.cpp):
///
///   PASS 1 (declareFunction): creates every function's llvm::Function
///   signature (parameter/return types, KAI name) with NO body, recording
///   `SymbolId -> llvm::Function*` in `functions_`.
///
///   PASS 2 (defineFunction): lowers every function's body, now able to
///   CreateCall any function from PASS 1 regardless of declaration order
///   - this is what makes forward calls, recursion, and (frontend-
///   permitting) mutual recursion work without any lazy "create callee on
///   first use" scheme.
///
/// M1-M4 support:
///
///     fn name(param: <type>, ...) -> <primitive scalar type | ()> {
///         <statement>*
///     }
///
/// - any number of parameters, each a primitive scalar type lowerType()
///   models - bound to entry-block storage exactly like an ordinary local
///   (see defineFunction()), so IdentifierExpr treats Local and Parameter
///   identically once bound.
/// - a flat sequence of VarDeclStmt / ExprStmt / ReturnStmt / (trivially)
///   nested BlockStmt statements, processed in source order - see
///   generateBlock()/generateStatement(). IfStmt/WhileStmt/ForStmt remain
///   explicit failures until statement-level control-flow lowering exists
///   (M5).
/// - expressions: integer/float/bool literals; parens; unary Negate/Not;
///   binary arithmetic/comparison/equality operators; SHORT-CIRCUIT `&&`/
///   `||` (see lowerLogicalExpr() - this is now final KAI language
///   semantics, not provisional backend behavior); identifier loads of a
///   resolved Local or Parameter; `identifier = value` assignment to a
///   resolved, mutable Local; and direct (or transparently-parenthesized)
///   user-function calls, including recursive and forward calls - see
///   lowerExpr()/lowerCallExpr() for the exact per-ExprKind rules.
/// - a function whose body's final reachable block has no terminator
///   after lowering emits `ret void` if its return type is Unit (this is
///   NOT inventing KAI semantics: Unit functions are already allowed to
///   fall through by frontend semantics - LLVM merely requires every
///   defined block to terminate); for a concrete non-Unit return type
///   this is instead a generation failure, since ControlFlowAnalyzer
///   already guarantees such a function's body always returns - reaching
///   this path means lowering itself failed to preserve that guarantee,
///   never a case to synthesize a fabricated return value for.
///
/// Any other function shape, statement kind, or expression kind causes
/// generate() to fail explicitly (return false) rather than emit partial
/// or malformed LLVM IR - see generate()'s own documentation. Builtin
/// calls, non-direct/first-class-function-value calls, method calls,
/// control flow (if/while/for), Range, a Unit-typed local variable (LLVM
/// has no storable void value - see generateVarDeclStmt()), and every
/// non-primitive-scalar semantic Type (references, arrays, str/String,
/// structs, enums, generics, Result, Option, ...) are explicitly deferred
/// to later LLVM codegen milestones (M5+).
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
    /// generate() call) via the two-pass declare-then-define strategy
    /// documented on this class, then runs llvm::verifyModule() on the
    /// result. Returns true only if the entire file lowered successfully
    /// AND the module verified; on false, the previous module() is
    /// discarded (reset to empty) so a caller can never read a partially-
    /// generated or unverified module by calling module() after a failed
    /// generate() - see module()'s precondition below.
    bool generate(const ast::SourceFile& file, const semantic::SemanticModel& model);

    /// The module produced by the most recent successful generate() call.
    ///
    /// Precondition: the most recent generate() call returned true.
    const llvm::Module& module() const;

private:
    bool declareTopLevelDecl(const ast::Decl& decl, const semantic::SemanticModel& model);
    bool defineTopLevelDecl(const ast::Decl& decl, const semantic::SemanticModel& model);

    /// PASS 1: creates `fn`'s llvm::Function signature only (parameter/
    /// return types lowered from FunctionSignature, KAI source name
    /// preserved, readable argument names assigned from source parameter
    /// names) and records it in `functions_`. Never lowers the body.
    /// Fails explicitly for any parameter or return semantic Type
    /// lowerType() does not model - never a re-resolution of the
    /// parameter/return TypeSyntax.
    bool declareFunction(const ast::FunctionDecl& fn, const semantic::SemanticModel& model);

    /// PASS 2: looks up `fn`'s already-declared llvm::Function (created
    /// in PASS 1 - asserted present, since generate() aborts entirely if
    /// PASS 1 ever fails for any function), creates its entry block,
    /// binds every parameter to entry-block storage exactly like an
    /// ordinary local (`Parameter SymbolId -> AllocaInst*` in the SAME
    /// `locals_` table Local declarations use - no second parameter-load
    /// architecture), lowers the body, and applies the function
    /// fallthrough policy documented on this class.
    bool defineFunction(const ast::FunctionDecl& fn, const semantic::SemanticModel& model);

    /// Lowers a flat statement sequence into the CURRENT LLVM BasicBlock
    /// (`builder`'s insertion point, re-read fresh on every iteration -
    /// never cached, since a short-circuit `&&`/`||` expression lowered
    /// by an earlier statement in this same sequence may have moved it) -
    /// a nested BlockStmt recurses into the SAME block/builder rather
    /// than creating a new one, since M4 still has no STATEMENT-level
    /// control flow to justify one (M5). Stops (without failing) the
    /// moment the current block already has a terminator - i.e. a prior
    /// ReturnStmt already ran - so no instruction is ever appended after
    /// a terminator; this is not an unreachable-code diagnostic, just
    /// where this pass chooses to stop lowering a flat block.
    bool generateBlock(const ast::BlockStmt& block, llvm::Function& function, llvm::IRBuilder<>& builder,
                        const semantic::SemanticModel& model);

    /// Exhaustive over ast::StmtKind. If/While/For are explicit failures
    /// (statement-level control flow is not yet lowerable) - never
    /// silently skipped.
    bool generateStatement(const ast::Stmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
                            const semantic::SemanticModel& model);

    /// Allocates entry-block storage for a `let`/`mut` local, lowers its
    /// initializer, and records `SymbolId -> AllocaInst*` in `locals_`.
    /// Reads the local's Symbol Type directly (`model.symbol(id).type`) -
    /// this is TypeChecker's own already-inferred-or-annotated type,
    /// never re-derived from the initializer or the TypeSyntax here (an
    /// inferred `let x = 40` and an annotated `let x: i64 = 40` are
    /// handled by the exact same code path). Fails explicitly - never
    /// allocating anything - for a Unit-typed local (LLVM void is not a
    /// storable value; see this class's own header comment) or any other
    /// semantic Type lowerType() does not model.
    bool generateVarDeclStmt(const ast::VarDeclStmt& varDecl, llvm::Function& function, llvm::IRBuilder<>& builder,
                              const semantic::SemanticModel& model);

    /// `y = y + 1` (or `do_work()`) as a statement: lowers the expression
    /// and discards its result (a successful Unit-valued expression, or
    /// any other value-producing expression used solely for its side
    /// effect). Only lowering failure is statement failure.
    bool generateExprStmt(const ast::ExprStmt& stmt, llvm::IRBuilder<>& builder, const semantic::SemanticModel& model);

    /// A bare `return` (Unit - only valid when the enclosing function's
    /// LLVM return type is void, defensively re-checked here even though
    /// TypeChecker already guarantees it) emits `ret void`; a valued
    /// `return expr` lowers `expr` and emits `ret <value>` against
    /// whichever block `lowerExpr()` actually leaves current (never
    /// assumed to still be the block this method started in - see this
    /// class's own header comment on short-circuit control flow).
    bool generateReturnStmt(const ast::ReturnStmt& stmt, llvm::IRBuilder<>& builder,
                             const semantic::SemanticModel& model);

    /// The central expression dispatcher: lowers any value-producing
    /// expression `lowerExpr()` currently supports into one LLVM SSA
    /// value, or std::nullopt on failure, or an explicit
    /// std::optional<llvm::Value*>{nullptr} for a successful Unit-valued
    /// expression (AssignmentExpr, or a call to a Unit-returning
    /// function - see lowerAssignmentExpr()/lowerCallExpr()).
    /// std::nullopt (never a nullable `llvm::Value*` alone) is what makes
    /// that distinction unambiguous. Reads `model.typeOf(expr)` as
    /// semantic ground truth for every dispatch decision - never
    /// re-infers a type independently. Fails (returns std::nullopt) for
    /// an Error/Unresolved semantic Type, or for any ExprKind this
    /// milestone does not lower (ArrayLiteral, Index, Member, Unit,
    /// ErrorPropagation).
    ///
    /// IMPORTANT: may move `builder`'s insertion point to a different
    /// BasicBlock than the one current when it was called (short-circuit
    /// `&&`/`||` creates new blocks - see lowerLogicalExpr()). Every
    /// caller must read `builder.GetInsertBlock()` fresh afterward rather
    /// than assuming its own previously-known block is still current -
    /// every caller in this class already does so.
    std::optional<llvm::Value*> lowerExpr(const ast::Expr& expr, const semantic::SemanticModel& model,
                                           llvm::IRBuilder<>& builder);

    std::optional<llvm::Value*> lowerLiteralExpr(const ast::LiteralExpr& literal, semantic::Type type,
                                                  llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerUnaryExpr(const ast::UnaryExpr& unary, const semantic::SemanticModel& model,
                                                llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerBinaryExpr(const ast::BinaryExpr& binary, const semantic::SemanticModel& model,
                                                 llvm::IRBuilder<>& builder);

    /// FINAL KAI semantics (M4): `&&`/`||` are short-circuit, not eager -
    /// see this class's own header comment. Builds real control flow:
    ///
    ///     lhs = lower(binary.left())
    ///     lhsEndBlock = builder.GetInsertBlock()   // AFTER lowering lhs
    ///     condbr lhs, rhsBlock, mergeBlock   // && - flipped for ||
    ///
    ///     rhsBlock:
    ///       rhs = lower(binary.right())
    ///       rhsEndBlock = builder.GetInsertBlock() // AFTER lowering rhs
    ///       br mergeBlock
    ///
    ///     mergeBlock:
    ///       phi i1 [false, lhsEndBlock], [rhs, rhsEndBlock]   // && - true/flipped for ||
    ///
    /// `lhsEndBlock`/`rhsEndBlock` are captured AFTER lowering their
    /// respective operand - NEVER the block that existed before lowering,
    /// and never blindly the block this method itself created - because
    /// lhs/rhs may themselves be a nested short-circuit expression that
    /// moves the builder again (e.g. `a && (b || c)`). This is the exact
    /// correctness requirement a PHI's incoming-block list has.
    std::optional<llvm::Value*> lowerLogicalExpr(const ast::BinaryExpr& binary, const semantic::SemanticModel& model,
                                                  llvm::IRBuilder<>& builder);

    /// A resolved Local's or Parameter's current value:
    /// `model.resolution(identifier)` (never identifier source text)
    /// finds the SymbolId, `locals_` finds its storage slot (the SAME
    /// table for both kinds - Parameter binding in defineFunction() uses
    /// it exactly like an ordinary local declaration does), and the
    /// slot's own allocated LLVM type drives the CreateLoad - never
    /// independently re-lowered from `model.typeOf()`, so a load's type
    /// can never drift from the alloca it reads. Fails explicitly for an
    /// unresolved identifier, or a Function/Builtin identifier (no
    /// modeled value - no first-class Function Type exists).
    std::optional<llvm::Value*> lowerIdentifierExpr(const ast::IdentifierExpr& identifier,
                                                     const semantic::SemanticModel& model,
                                                     llvm::IRBuilder<>& builder);

    /// `identifier = value` (through transparent ParenExpr wrappers only,
    /// mirroring TypeChecker's own unwrapAssignmentTargetIdentifier()
    /// structural-only unwrap - never a semantic decision, since
    /// TypeChecker has already validated this exact target/value pair).
    /// Never validates mutability or target-shape itself - a successfully
    /// type-checked AssignmentExpr already guarantees the target is a
    /// mutable, storage-backed Local (Parameters are always immutable per
    /// GRAMMAR.md §10, so TypeChecker never lets one reach here) - so "no
    /// slot found" is sufficient, on its own, to reject every
    /// unsupported case. Returns std::optional<llvm::Value*>{nullptr} -
    /// Unit success - on a successful store; NEVER the stored value
    /// itself, since KAI assignment is not a C-style value-producing
    /// expression.
    std::optional<llvm::Value*> lowerAssignmentExpr(const ast::AssignmentExpr& assignment,
                                                     const semantic::SemanticModel& model, llvm::IRBuilder<>& builder);

    /// A direct (or transparently-parenthesized) user-function call -
    /// the current frontend-supported call subset. Resolution is
    /// exclusively `model.resolution()`/`SymbolKind` (never identifier
    /// text); the callee's already-declared llvm::Function (from PASS 1 -
    /// see `functions_`) is found by SymbolId, which is what actually
    /// makes forward and recursive calls work with no special-casing
    /// here. A CallExpr resolving to SymbolKind::Builtin, or any callee
    /// shape that isn't a direct/parenthesized identifier, fails
    /// explicitly - builtins and first-class/method call forms remain
    /// deferred. Returns std::optional<llvm::Value*>{nullptr} - Unit
    /// success, never a fabricated LLVM value - for a call to a
    /// Unit-returning function.
    std::optional<llvm::Value*> lowerCallExpr(const ast::CallExpr& call, semantic::Type type,
                                               const semantic::SemanticModel& model, llvm::IRBuilder<>& builder);

    /// Maps a semantic::Type to its LLVM counterpart, covering every
    /// primitive scalar kind this milestone's Type models (Unit and the
    /// integer/float/bool kinds) - a small, already-trivial table.
    /// Returns nullptr for Unresolved, Error, and Char - never a
    /// fabricated placeholder LLVM type - so an unsupported semantic
    /// Type causes an explicit generation failure at the call site
    /// instead. Note Unit maps to LLVM void here (a legitimate function
    /// return type) - callers that cannot accept a storable value (e.g.
    /// generateVarDeclStmt()) must reject void themselves; lowerType()
    /// does not encode "is this usable as a local's storage type" policy.
    llvm::Type* lowerType(semantic::Type type);

    /// Allocates `type`-typed storage at the START of `function`'s entry
    /// block, regardless of `builder`'s own current insertion point -
    /// grouping every local's/parameter's alloca at the top of entry
    /// (mem2reg-friendly IR) while ordinary instructions (an
    /// initializer's/parameter's CreateStore, everything else) still emit
    /// at their normal, in-order position via `builder`. `name` is a
    /// human-readable LLVM IR name only (the source spelling) - never
    /// used for lookup; SymbolId is storage identity (see locals_).
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function& function, llvm::Type* type, llvm::StringRef name);

    /// Linear scan, not std::unordered_map/std::map: SymbolId exposes no
    /// public hash or ordering (only operator==, deliberately - see
    /// Symbol.hpp), and inventing one merely to get a faster container
    /// would be exactly the kind of speculative cross-layer API addition
    /// this milestone was told not to make. A handful of locals/
    /// parameters per function makes a linear scan the smallest useful
    /// representation, not a performance concern. Cleared at the start
    /// of every defineFunction() call - never reused across functions.
    llvm::AllocaInst* findLocalSlot(semantic::SymbolId id) const;

    /// Linear scan, same rationale as findLocalSlot() - and for the same
    /// SymbolId-API reason. Populated once per function during PASS 1
    /// (declareFunction()) and never cleared - unlike `locals_`, callee
    /// identity must remain visible across every function's PASS 2 body,
    /// which is exactly what makes forward and recursive calls work.
    llvm::Function* findFunction(semantic::SymbolId id) const;

    const SourceManager& sources_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    std::vector<std::pair<semantic::SymbolId, llvm::AllocaInst*>> locals_;
    std::vector<std::pair<semantic::SymbolId, llvm::Function*>> functions_;
};

} // namespace kai::codegen
