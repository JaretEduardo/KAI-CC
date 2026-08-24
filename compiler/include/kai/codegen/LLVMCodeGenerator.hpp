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

/// LLVM CODEGEN MILESTONE 1+2+3: Minimal LLVM Module + Function + Integer
/// Return (M1), Primitive Expression Lowering (M2), Local Variables +
/// Identifier Loads + Assignment (M3).
///
/// LLVMCodeGenerator lowers an already-fully-checked AST into an
/// llvm::Module:
///
///     AST + SemanticModel -> LLVM IR
///
/// with no HIR stage (a deliberate, MVP-deadline-driven decision - HIR
/// remains post-MVP work). It performs NO semantic work of its own: no
/// name resolution, no TypeSyntax re-resolution, no type checking, no
/// type inference, no mutability/assignment-target validation, and it
/// never mutates SemanticModel or the AST. SemanticModel's declaration/
/// resolution mappings and Symbol Types are read-only semantic ground
/// truth here, exactly as SemanticAnalyzer/TypeChecker/ControlFlowAnalyzer
/// already left them - the same frontend contract, with this as a final,
/// non-mutating consumer.
///
/// Callers must run SemanticAnalyzer::analyze(), TypeChecker::check(),
/// and ControlFlowAnalyzer::check() on `file`/`model` before calling
/// generate() - this class assumes the frontend already succeeded
/// (SemanticModel::errors() is empty) and does not re-validate that.
///
/// M1-M3 support exactly one function shape:
///
///     fn name() -> <primitive scalar type> {
///         <statement>*
///     }
///
/// - zero parameters only (FunctionSignature::parameterTypes must be
///   empty) - parameter lowering is explicitly out of scope, not merely
///   unimplemented by oversight.
/// - a flat sequence of VarDeclStmt / ExprStmt / ReturnStmt / (trivially)
///   nested BlockStmt statements, processed in source order - see
///   generateBlock()/generateStatement() in LLVMCodeGenerator.cpp. A
///   ReturnStmt terminates lowering of the current block (no instructions
///   are ever emitted after an LLVM terminator); IfStmt/WhileStmt/ForStmt
///   remain explicit failures until control-flow lowering exists.
/// - expressions: integer/float/bool literals; parens; unary Negate/Not;
///   binary arithmetic/comparison/equality/logical operators; identifier
///   loads of a resolved Local; and `identifier = value` assignment to a
///   resolved, mutable Local - see lowerExpr() for the exact per-ExprKind
///   rules and LLVM opcode/predicate selection.
///
/// Any other function shape, statement kind, or expression kind causes
/// generate() to fail explicitly (return false) rather than emit partial
/// or malformed LLVM IR - see generate()'s own documentation. Function
/// parameters, calls, control flow (if/while/for), Range, a Unit-typed
/// local variable (LLVM has no storable void value - see
/// generateVarDeclStmt()), and every non-primitive-scalar semantic Type
/// (references, arrays, str/String, structs, enums, generics, Result,
/// Option, ...) are explicitly deferred to later LLVM codegen milestones
/// (M4+).
///
/// `&&`/`||` still lower as eager (non-short-circuiting) LLVM `and`/`or`
/// on i1 operands - see lowerBinaryExpr()'s own comment. This remains
/// acceptable ONLY through M3: the newly-supported AssignmentExpr is
/// itself Unit-typed, so it can never satisfy `&&`/`||`'s Bool-operand
/// requirement, meaning no side-effecting expression can reach logical
/// operand position yet even with assignment in the language. This
/// changes the moment M4 adds Bool-returning function calls (which CAN
/// have observable side effects) - the short-circuit-vs-eager decision
/// MUST be resolved and correctly implemented before call lowering merges,
/// not deferred again.
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

    /// Lowers a flat statement sequence into the CURRENT LLVM BasicBlock
    /// (`builder`'s insertion point) - a nested BlockStmt (M3 spec §15)
    /// recurses into the SAME block/builder rather than creating a new
    /// one, since M3 has no control flow to justify a new block. Stops
    /// (without failing) the moment the current block already has a
    /// terminator - i.e. a prior ReturnStmt already ran - so no
    /// instruction is ever appended after a terminator; this is not an
    /// unreachable-code diagnostic, just where M3 chooses to stop
    /// lowering a flat block, mirroring ControlFlowAnalyzer's own
    /// "further statements are still fully checked elsewhere, never
    /// re-diagnosed here" stance.
    bool generateBlock(const ast::BlockStmt& block, llvm::Function& function, llvm::IRBuilder<>& builder,
                        const semantic::SemanticModel& model);

    /// Exhaustive over ast::StmtKind. If/While/For are explicit failures
    /// (control flow is not yet lowerable) - never silently skipped.
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

    /// `y = y + 1` as a statement: lowers the expression and discards its
    /// result (a successful Unit-valued AssignmentExpr, or any other
    /// value-producing expression used solely for its store side effect).
    /// Only lowering failure is statement failure.
    bool generateExprStmt(const ast::ExprStmt& stmt, llvm::IRBuilder<>& builder, const semantic::SemanticModel& model);

    bool generateReturnStmt(const ast::ReturnStmt& stmt, llvm::IRBuilder<>& builder,
                             const semantic::SemanticModel& model);

    /// The central expression dispatcher: lowers any value-producing
    /// expression `lowerExpr()` currently supports into one LLVM SSA
    /// value, or std::nullopt on failure, or an explicit
    /// std::optional<llvm::Value*>{nullptr} for a successful Unit-valued
    /// expression (M3: exactly AssignmentExpr - see lowerAssignmentExpr()).
    /// std::nullopt (never a nullable `llvm::Value*` alone) is what makes
    /// that distinction unambiguous - the exact ambiguity a bare nullable
    /// pointer could not represent. Reads `model.typeOf(expr)` as
    /// semantic ground truth for every dispatch decision - never
    /// re-infers a type independently. Fails (returns std::nullopt) for
    /// an Error/Unresolved semantic Type, or for any ExprKind this
    /// milestone does not lower (Call, ArrayLiteral, Index, Member, Unit,
    /// ErrorPropagation).
    std::optional<llvm::Value*> lowerExpr(const ast::Expr& expr, const semantic::SemanticModel& model,
                                           llvm::IRBuilder<>& builder);

    std::optional<llvm::Value*> lowerLiteralExpr(const ast::LiteralExpr& literal, semantic::Type type,
                                                  llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerUnaryExpr(const ast::UnaryExpr& unary, const semantic::SemanticModel& model,
                                                llvm::IRBuilder<>& builder);
    std::optional<llvm::Value*> lowerBinaryExpr(const ast::BinaryExpr& binary, const semantic::SemanticModel& model,
                                                 llvm::IRBuilder<>& builder);

    /// A resolved Local's current value: `model.resolution(identifier)`
    /// (never identifier source text) finds the SymbolId, `locals_` finds
    /// its storage slot, and the slot's own allocated LLVM type drives
    /// the CreateLoad - never independently re-lowered from
    /// `model.typeOf()`, so a load's type can never drift from the
    /// alloca it reads. Fails explicitly for an unresolved identifier, a
    /// Function/Builtin identifier (no modeled value), or - most
    /// importantly for M3 - a Parameter: parameters get no storage slot
    /// at all yet, so this fails exactly like a missing Local slot would.
    std::optional<llvm::Value*> lowerIdentifierExpr(const ast::IdentifierExpr& identifier,
                                                     const semantic::SemanticModel& model,
                                                     llvm::IRBuilder<>& builder);

    /// `identifier = value` (through transparent ParenExpr wrappers only,
    /// mirroring TypeChecker's own unwrapAssignmentTargetIdentifier()
    /// structural-only unwrap - never a semantic decision, since
    /// TypeChecker has already validated this exact target/value pair).
    /// Never validates mutability or target-shape itself - a successfully
    /// type-checked AssignmentExpr already guarantees the target is a
    /// mutable, storage-backed Local (see this method's own body for why
    /// "no slot found" is therefore sufficient, on its own, to reject
    /// every case M3 does not support: Parameter, Function/Builtin, and
    /// Member/Index/other targets never get a slot in the first place).
    /// Returns std::optional<llvm::Value*>{nullptr} - Unit success - on a
    /// successful store; NEVER the stored value itself, since KAI
    /// assignment is not a C-style value-producing expression.
    std::optional<llvm::Value*> lowerAssignmentExpr(const ast::AssignmentExpr& assignment,
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
    /// grouping every local's alloca at the top of entry (mem2reg-
    /// friendly IR) while ordinary instructions (the initializer's
    /// CreateStore, everything else) still emit at their normal, in-order
    /// position via `builder`. `name` is a human-readable LLVM IR name
    /// only (the source spelling) - never used for lookup; SymbolId is
    /// storage identity (see locals_).
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function& function, llvm::Type* type, llvm::StringRef name);

    /// Linear scan, not std::unordered_map/std::map: SymbolId exposes no
    /// public hash or ordering (only operator==, deliberately - see
    /// Symbol.hpp), and inventing one merely to get a faster container
    /// would be exactly the kind of speculative cross-layer API addition
    /// this milestone was told not to make. A handful of locals per
    /// function makes a linear scan the smallest useful representation,
    /// not a performance concern. Cleared at the start of every
    /// generateFunction() call - never reused across functions.
    llvm::AllocaInst* findLocalSlot(semantic::SymbolId id) const;

    const SourceManager& sources_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    std::vector<std::pair<semantic::SymbolId, llvm::AllocaInst*>> locals_;
};

} // namespace kai::codegen
