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

/// LLVM CODEGEN MILESTONE 1+2+3+4+5+6: Minimal LLVM Module + Function +
/// Integer Return (M1), Primitive Expression Lowering (M2), Local
/// Variables + Identifier Loads + Assignment (M3), Parameters + Function
/// Calls + Recursion + FINAL short-circuit &&/|| (M4), If/Else/Else-If +
/// While statement-level control flow (M5, ForStmt still deferred), and a
/// minimal `print` builtin lowered to a tiny native runtime ABI (M6,
/// `panic`/`assert` still deferred).
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
/// - a sequence of VarDeclStmt / ExprStmt / ReturnStmt / (trivially)
///   nested BlockStmt / IfStmt (with any number of `else if` branches and
///   an optional final `else`) / WhileStmt statements, in source order -
///   see generateBlock()/generateStatement()/generateIfStmt()/
///   generateWhileStmt(). ForStmt remains an explicit failure until
///   iteration lowering exists (M6+).
/// - expressions: integer/float/bool literals; parens; unary Negate/Not;
///   binary arithmetic/comparison/equality operators; SHORT-CIRCUIT `&&`/
///   `||` (see lowerLogicalExpr() - this is now final KAI language
///   semantics, not provisional backend behavior); identifier loads of a
///   resolved Local or Parameter; `identifier = value` assignment to a
///   resolved, mutable Local; and direct (or transparently-parenthesized)
///   user-function calls, including recursive and forward calls - see
///   lowerExpr()/lowerCallExpr() for the exact per-ExprKind rules.
/// - a function whose body lowers to StatementResult::FallsThrough (its
///   final reachable block has no terminator - see generateBlock()'s own
///   StatementResult) emits `ret void` if its return type is Unit (this is
///   NOT inventing KAI semantics: Unit functions are already allowed to
///   fall through by frontend semantics - LLVM merely requires every
///   defined block to terminate); for a concrete non-Unit return type
///   this is instead a generation failure, since ControlFlowAnalyzer
///   already guarantees such a function's body always returns - reaching
///   this path means lowering itself failed to preserve that guarantee,
///   never a case to synthesize a fabricated return value for. A body
///   that lowers to StatementResult::Terminated (every path already ends
///   in an explicit `return`, possibly via if/else) needs no further
///   action here at all.
///
/// M6 additionally recognizes exactly one Builtin call: `print(x)`,
/// resolved via `model.resolution()`/SymbolKind::Builtin (a user
/// declaration shadowing `print` resolves as an ordinary
/// SymbolKind::Function instead - see STANDARD_LIBRARY.md §3 - and is
/// lowered exactly like any other user-function call, never hijacked by
/// this builtin path). `print(x)` lowers to a call into this project's
/// own tiny native runtime ABI (runtime/kai_runtime.h/.c) selected by
/// `x`'s own semantic Type - see lowerPrintCall() for the exact supported
/// Type set and runtime function names.
///
/// Any other function shape, statement kind, or expression kind causes
/// generate() to fail explicitly (return false) rather than emit partial
/// or malformed LLVM IR - see generate()'s own documentation. Every other
/// Builtin call (`panic`, `assert`, ...), non-direct/first-class-
/// function-value calls, method calls, `for` iteration, Range, a
/// Unit-typed local variable (LLVM has no storable void value - see
/// generateVarDeclStmt()), and every non-primitive-scalar semantic Type
/// (references, arrays, str/String, structs, enums, generics, Result,
/// Option, ...) are explicitly deferred to later LLVM codegen milestones
/// (M7+).
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
    /// A lowered statement's effect on the CURRENT LLVM BasicBlock, needed
    /// starting M5 (IfStmt/WhileStmt) because "lowered successfully" is no
    /// longer just one outcome (bool was sufficient through M4, where only
    /// a ReturnStmt could ever terminate a block):
    ///
    ///   Failed:      codegen failed outright.
    ///   FallsThrough: lowered successfully, and the block `builder` now
    ///                 points at is a real, unterminated block execution
    ///                 may continue into (the common case; also every
    ///                 WhileStmt, which may run zero iterations - see
    ///                 generateWhileStmt()).
    ///   Terminated:   lowered successfully, but every path out of this
    ///                 statement already ended in an LLVM terminator (e.g.
    ///                 both arms of an if/else return) - there is no
    ///                 continuation on this path, and the caller must not
    ///                 append anything after it (mirrors
    ///                 ControlFlowAnalyzer::FlowResult::AlwaysReturns -
    ///                 see ControlFlowAnalyzer.cpp's own analyzeIfStmt()).
    ///
    /// Deliberately NOT a general CFG-framework type - just the smallest
    /// enum that lets generateBlock()/defineFunction() distinguish "keep
    /// lowering into this block" from "this path is done" without
    /// fabricating an unreachable merge block to force a bool answer.
    enum class StatementResult {
        Failed,
        FallsThrough,
        Terminated,
    };

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
    /// (`builder`'s insertion point, re-read fresh via generateStatement()
    /// on every iteration - never cached, since a short-circuit `&&`/`||`
    /// expression, or a nested IfStmt/WhileStmt, lowered by an earlier
    /// statement in this same sequence may have moved it) - a nested
    /// BlockStmt recurses into the SAME block/builder rather than creating
    /// a new one (KAI has no lexical-scope-only `{ }` statement - see
    /// StmtKind::Block's own case in generateStatement()). Stops lowering
    /// (without failing) the moment a statement returns
    /// StatementResult::Terminated - e.g. a ReturnStmt, or an if/else
    /// whose every arm returns - so no instruction is ever appended after
    /// an LLVM terminator; this is not an unreachable-code diagnostic
    /// (TypeChecker/ControlFlowAnalyzer already fully checked any
    /// statements that follow), just where this pass stops lowering a
    /// flat block. Returns whatever StatementResult the last-lowered
    /// statement produced (FallsThrough if `block` is empty).
    StatementResult generateBlock(const ast::BlockStmt& block, llvm::Function& function, llvm::IRBuilder<>& builder,
                                   const semantic::SemanticModel& model);

    /// Exhaustive over ast::StmtKind. ForStmt remains an explicit failure
    /// (statement-level iteration is not yet lowerable - M6+) - never
    /// silently skipped. VarDecl/Expr/Return wrap their existing bool
    /// result into a StatementResult (VarDecl/Expr -> FallsThrough on
    /// success, Return -> Terminated on success, either -> Failed on
    /// failure); If/While compute their own StatementResult directly (see
    /// generateIfStmt()/generateWhileStmt()).
    StatementResult generateStatement(const ast::Stmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
                                       const semantic::SemanticModel& model);

    /// IfStmt lowering (M5), covering plain `if`, `if`/`else`, and
    /// `else if` chains uniformly: `branches()[0]` is the initial `if`;
    /// recurses into `branches()[index + 1]` for each `else if` (lowered
    /// AS a nested if inside what would otherwise be the plain `else`
    /// block - see this method's .cpp comment - never special-cased on
    /// source spelling), and finally into `elseClause()->body()` (an
    /// ordinary block) once `index` reaches the last branch. Condition is
    /// lowered via lowerExpr() and its ACTUAL post-lowering insertion
    /// block is what the CondBr is built from (a short-circuit `&&`/`||`
    /// condition may itself have created blocks - see lowerLogicalExpr()
    /// and this class's own header comment on builder insertion points).
    /// A branch with no `else` at all always creates a reachable merge
    /// block (the false-condition edge targets it directly), so a plain
    /// `if` with no `else` always yields StatementResult::FallsThrough. An
    /// `if`/`else` where both arms yield StatementResult::Terminated
    /// creates NO merge block (erased immediately after being created,
    /// rather than leaving it unterminated for the verifier to reject, or
    /// fabricating a bogus instruction to satisfy it - M5 spec §4) and
    /// this method returns StatementResult::Terminated instead, exactly
    /// mirroring ControlFlowAnalyzer::analyzeIfStmt()'s own
    /// FlowResult::AlwaysReturns rule (never re-implemented here as a
    /// second independent completeness analysis - this is backend CFG
    /// construction arriving at the same, already-frontend-guaranteed,
    /// conclusion). Fails explicitly for a missing/non-real/non-i1
    /// condition value.
    StatementResult generateIfStmt(const ast::IfStmt& stmt, std::size_t branchIndex, llvm::Function& function,
                                    llvm::IRBuilder<>& builder, const semantic::SemanticModel& model);

    /// WhileStmt lowering (M5): condition/body/exit blocks per this
    /// class's own header comment. The condition is lowered fresh on
    /// every iteration (its own dedicated `while.cond` block, entered via
    /// an unconditional branch from wherever `builder` starts, and via a
    /// back-edge from the body when the body falls through) - never
    /// hoisted or assumed loop-invariant. The CondBr is built from
    /// `lowerExpr(condition)`'s ACTUAL post-lowering insertion block, same
    /// reasoning as generateIfStmt(). If the body yields
    /// StatementResult::Terminated (e.g. an unconditional `return` inside
    /// the loop body), no back-edge is emitted - but the loop's own exit
    /// block is still reachable (the condition's false edge always
    /// targets it directly), matching ControlFlowAnalyzer's own
    /// deliberately-conservative stance that a `while` loop can never be
    /// proven to execute even once without constant-condition reasoning
    /// (which neither the frontend nor this backend performs) - so
    /// WhileStmt as a whole ALWAYS returns StatementResult::FallsThrough,
    /// never Terminated, regardless of the body. Fails explicitly for a
    /// missing/non-real/non-i1 condition value.
    StatementResult generateWhileStmt(const ast::WhileStmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
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
    /// an Error semantic Type, or for any ExprKind this milestone does
    /// not lower (ArrayLiteral, Index, Member, Unit, ErrorPropagation).
    ///
    /// Unresolved is REJECTED with exactly one narrow, structurally-
    /// identified exception (M6): a CallExpr whose direct callee resolves
    /// (via `model.resolution()`/SymbolKind::Builtin - never identifier
    /// text) to `print`. TypeChecker's checkBuiltinCall() always records
    /// Type::unresolved() for a builtin CALL itself (STANDARD_LIBRARY.md
    /// §3: builtin call signatures are not yet committed), regardless of
    /// its arguments' own concrete types - so without this exception a
    /// recognized, supported `print(x)` could never reach lowerCallExpr()
    /// at all. This is deliberately NOT "allow Unresolved for any Call":
    /// every other Unresolved CallExpr shape (an unsupported Builtin, a
    /// non-function-typed callee, a deferred callee expression - see
    /// TypeChecker.cpp's checkCallExpr()) is still rejected here exactly
    /// as before. See the anonymous-namespace `isPrintBuiltinCall()`
    /// helper in LLVMExpressionLowering.cpp for the exact structural
    /// check.
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

    /// A direct (or transparently-parenthesized) call - the current
    /// frontend-supported call subset. Resolution is exclusively
    /// `model.resolution()`/`SymbolKind` (never identifier text). Any
    /// callee shape that isn't a direct/parenthesized identifier fails
    /// explicitly - first-class/method call forms remain deferred.
    ///
    ///   SymbolKind::Function: the callee's already-declared
    ///   llvm::Function (from PASS 1 - see `functions_`) is found by
    ///   SymbolId, which is what actually makes forward and recursive
    ///   calls work with no special-casing here. Returns
    ///   std::optional<llvm::Value*>{nullptr} - Unit success, never a
    ///   fabricated LLVM value - for a call to a Unit-returning function.
    ///
    ///   SymbolKind::Builtin (M6): delegates to lowerBuiltinCallExpr() -
    ///   `type` (the CALL's own semantic Type) is not used for this path,
    ///   since TypeChecker's checkBuiltinCall() always records
    ///   Type::unresolved() for a builtin CallExpr itself, regardless of
    ///   its arguments' own concrete types (see TypeChecker.cpp) - see
    ///   lowerExpr()'s own comment on the narrow Unresolved-gate exception
    ///   this requires.
    std::optional<llvm::Value*> lowerCallExpr(const ast::CallExpr& call, semantic::Type type,
                                               const semantic::SemanticModel& model, llvm::IRBuilder<>& builder);

    /// Dispatches a CallExpr already confirmed (by lowerCallExpr(), via
    /// `model.resolution()`/SymbolKind - never identifier text) to resolve
    /// to `builtinSymbol` (SymbolKind::Builtin). M6 recognizes exactly one
    /// builtin by its resolved Symbol::name: `print` (see
    /// lowerPrintCall()). Every other recognized builtin (`panic`,
    /// `assert`, ...) fails generation explicitly - M6 spec §17: never a
    /// silently-skipped/no-op builtin call.
    std::optional<llvm::Value*> lowerBuiltinCallExpr(const ast::CallExpr& call, const semantic::Symbol& builtinSymbol,
                                                      const semantic::SemanticModel& model,
                                                      llvm::IRBuilder<>& builder);

    /// Lowers `print(x)` to a call into this project's own tiny native
    /// runtime ABI (runtime/kai_runtime.h/.c) rather than a libc printf
    /// formatting decision tree inlined into generated IR - KAI print
    /// semantics stay a small, closed dispatch over `x`'s own semantic
    /// Type (read from `model.typeOf()` - print has no committed
    /// FunctionSignature to validate against yet, see
    /// STANDARD_LIBRARY.md §3, so this IS the argument-count/argument-type
    /// validation for this one recognized builtin - deliberately narrow,
    /// not a general type checker):
    ///
    ///   exactly 1 argument, with a real, non-Error/non-Unresolved
    ///   semantic Type, required - anything else fails explicitly.
    ///
    ///   signed integer   -> sign-extend to i64,  call `kai_print_i64`
    ///   unsigned integer -> zero-extend to i64,  call `kai_print_u64`
    ///   Bool              -> zero-extend to i32,  call `kai_print_bool`
    ///   F32/F64           -> extend to double,    call `kai_print_f64`
    ///   anything else (Char, String, ...)         -> explicit failure
    ///
    /// Each runtime function is declared into the CURRENT module via
    /// `module_->getOrInsertFunction()` - this both creates the
    /// `declare` the first time a given ABI function is needed AND
    /// transparently reuses that exact same llvm::Function for every
    /// subsequent `print` call requiring it, so multiple `print(i64...)`
    /// calls in one module never produce duplicate declarations. Always
    /// returns std::optional<llvm::Value*>{nullptr} on success - print is
    /// an effectful, Unit-valued builtin (M6 spec §10): its call must
    /// never surface the runtime function's own (void) C ABI return as a
    /// KAI value.
    std::optional<llvm::Value*> lowerPrintCall(const ast::CallExpr& call, const semantic::SemanticModel& model,
                                                llvm::IRBuilder<>& builder);

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
