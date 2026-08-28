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
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kai::codegen {

/// LLVM CODEGEN MILESTONE 1+2+3+4+5+6: Minimal LLVM Module + Function +
/// Integer Return (M1), Primitive Expression Lowering (M2), Local
/// Variables + Identifier Loads + Assignment (M3), Parameters + Function
/// Calls + Recursion + FINAL short-circuit &&/|| (M4), If/Else/Else-If +
/// While statement-level control flow (M5), and a minimal `print` builtin
/// lowered to a tiny native runtime ABI (M6, `panic`/`assert` still
/// deferred). KAI LANGUAGE M6 (`for` + integer ranges, post-alpha.2 -
/// distinct from the LLVM codegen milestone numbering above) adds ForStmt
/// lowering for the one supported iterable form - a literal integer
/// `start..end` range - see generateForStmt(). KAI LANGUAGE M7B
/// (post-alpha.2) adds real execution for a LOCAL fixed-size array -
/// `[N x T]` storage, checked indexed reads/writes - see
/// generateArrayVarDeclStmt()/lowerArrayElementAddress().
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
///   an optional final `else`) / WhileStmt / ForStmt statements, in
///   source order - see generateBlock()/generateStatement()/
///   generateIfStmt()/generateWhileStmt()/generateForStmt(). ForStmt
///   lowering (KAI LANGUAGE M6, post-alpha.2) supports exactly one
///   iterable form: a literal integer `start..end` half-open range -
///   any other iterable shape is already rejected by TypeChecker
///   (SemanticErrorKind::UnsupportedForIterable), so it never reaches
///   codegen at all.
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
/// function-value calls, method calls, a bare Range as a first-class
/// runtime value (`let r = 0..10` - still unlowerable; only a `for`
/// loop's own `start..end` iterable is ever lowered, and only its two
/// endpoints, never the Range "value" itself - see generateForStmt()), a
/// Unit-typed local variable (LLVM has no storable void value - see
/// generateVarDeclStmt()), and every remaining non-primitive-scalar
/// semantic Type (references, structs, enums, generics, Result, Option,
/// ...) are explicitly deferred to later LLVM codegen milestones. As of
/// the Minimal String Literal Support milestone, Str (see Type::str()'s
/// own comment) is ALSO a lowerable scalar-like Type - a string literal,
/// an inferred `let`/`mut` local backed by one, and `print(x)` where `x`
/// is Str all lower successfully - but this is deliberately narrow: `str`
/// remains unspellable as a source type annotation, and `String`/`&str`/
/// concatenation/formatting/string methods remain entirely unmodeled. KAI
/// LANGUAGE M7B (post-alpha.2) additionally lowers a LOCAL fixed-size
/// array (`let`/`mut xs = [elem, ...]`) to real `[N x T]` LLVM storage
/// with checked indexed reads/writes (see generateArrayVarDeclStmt()/
/// lowerArrayElementAddress() and this class's own header note above) -
/// arrays as a function PARAMETER or RETURN type, and whole-array
/// assignment/copy (`let b = a` / `a = b`), remain explicitly deferred
/// (no array calling-convention/ABI exists, and whole-array Copy
/// semantics are an unreviewed language-design question - see
/// declareFunction()'s own explicit Array parameter/return guard and
/// lowerAssignmentExpr()'s/generateArrayVarDeclStmt()'s own explicit
/// whole-array guards, none of which are accidental omissions).
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

    /// RELEASE HARDENING M2: a small, deliberately narrow diagnostic-UX
    /// addition - NOT a general diagnostic framework. Set exactly once,
    /// at the specific site an explicitly-deferred (never silently
    /// skipped) AST construct causes generate() to fail - e.g. an
    /// array/slice parameter or return type declareFunction() cannot
    /// lower (see that method's own recordUnsupportedConstruct() calls).
    /// Left std::nullopt for every other failure path, including a
    /// genuine llvm::verifyModule() failure and any defensive/internal
    /// check - this is intentional: a caller (CompileCommand) uses its
    /// presence to show a specific, actionable message (e.g. "code
    /// generation is not yet supported for this parameter's type")
    /// instead of the generic "LLVM IR generation failed", but must
    /// NEVER attribute a real verifier failure to "an unsupported
    /// construct". Cleared at the start of every generate() call - never
    /// stale from a previous call.
    struct UnsupportedConstruct {
        std::string description;
        SourceSpan span;
    };
    const std::optional<UnsupportedConstruct>& unsupportedConstruct() const noexcept { return unsupportedConstruct_; }

    /// The module produced by the most recent successful generate() call.
    ///
    /// Precondition: the most recent generate() call returned true.
    const llvm::Module& module() const;

    /// Non-const overload (M7): the M7 native-executable pipeline
    /// mutates the generated module IN PLACE after generate() succeeds -
    /// native-entry ABI adaptation (see
    /// LLVMObjectEmitter::adaptNativeEntryPoint()) and target-triple/
    /// DataLayout assignment (see LLVMObjectEmitter::emit()) - before
    /// object emission. Same precondition as the const overload; existing
    /// callers that only ever read the module (every codegen test, and
    /// anything binding the result to `const llvm::Module&`) are
    /// unaffected by this addition.
    llvm::Module& module();

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

    /// Exhaustive over ast::StmtKind. VarDecl/Expr/Return wrap their
    /// existing bool result into a StatementResult (VarDecl/Expr ->
    /// FallsThrough on success, Return -> Terminated on success, either ->
    /// Failed on failure); If/While/For compute their own StatementResult
    /// directly (see generateIfStmt()/generateWhileStmt()/
    /// generateForStmt()) - never silently skipped.
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

    /// ForStmt lowering (KAI LANGUAGE M6, post-alpha.2): TypeChecker
    /// already guarantees `stmt.iterable()` is a BinaryExpr{Range} whose
    /// two endpoints share one lowerable concrete integer Type (the loop
    /// variable's own Symbol type, per checkForStmt()/checkIntegerRangeFor()
    /// in TypeChecker.cpp) - defensively re-checked here (never trusted
    /// blindly) via lowerType()/a structural cast, falling back to
    /// recordUnsupportedConstruct() on mismatch exactly like
    /// generateVarDeclStmt() does for its own local-type checks, even
    /// though the frontend makes this path practically unreachable today.
    ///
    /// Conceptual lowering (half-open `start <= i < end`):
    ///
    ///     induction = alloca <elementType>
    ///     store start, induction     // evaluated ONCE, in the preheader
    ///     endValue = <end>           // evaluated ONCE, in the preheader
    ///     br for.cond
    ///
    ///     for.cond:
    ///       i = load induction
    ///       br (i < endValue) ? for.body : for.end   // signed/unsigned
    ///                                                 // per elementType
    ///
    ///     for.body:
    ///       <bind the loop variable's SymbolId to `induction` in
    ///        locals_ - the SAME slot the condition/increment read/write,
    ///        so the body's own IdentifierExpr loads see the current
    ///        induction value with no separate PHI/copy>
    ///       <body>
    ///       next = add i, 1
    ///       store next, induction
    ///       br for.cond                // only if body FallsThrough
    ///
    ///     for.end:
    ///
    /// No SSA PHI node is used for the induction variable - it lives in
    /// ordinary alloca'd memory and is re-loaded every time it's read,
    /// exactly like every other KAI local this class already lowers
    /// (mirrors generateWhileStmt()'s own memory-based, no-PHI style,
    /// never introducing a second lowering strategy). `endValue` is
    /// computed exactly once, in the preheader, and reused as the SAME
    /// llvm::Value* in every `for.cond` iteration (it dominates
    /// `for.cond` since the preheader always branches into it first) -
    /// never re-lowered from `stmt.iterable()`'s right operand inside the
    /// loop. Half-open design: the condition alone (`i < end`) decides
    /// whether to enter/continue the body, so `start >= end` naturally
    /// yields zero iterations with no separate pre-check, and the
    /// induction variable is never incremented past `end` (no risk of
    /// signed/unsigned overflow at the boundary - see this method's own
    /// comment in the .cpp for the exact argument). Same "a loop can
    /// never be soundly proven to execute even once" stance as
    /// generateWhileStmt() - ForStmt as a whole ALWAYS returns
    /// StatementResult::FallsThrough, matching
    /// ControlFlowAnalyzer::analyzeStatement()'s own conservative
    /// FlowResult for `for`. Fails explicitly for a missing/non-real
    /// start/end value, a non-integer element type, or an iterable that
    /// isn't structurally a Range (defensive only - see above).
    StatementResult generateForStmt(const ast::ForStmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
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

    /// KAI LANGUAGE M7B: an array-typed local's own initialization path,
    /// dispatched to from generateVarDeclStmt() the moment `arrayType`
    /// (already lowered from the local's own Symbol Type) is an
    /// llvm::ArrayType. Supports EXACTLY `let`/`mut xs = [elem, ...]` (a
    /// possibly paren-wrapped ArrayLiteralExpr initializer, evaluated via
    /// lowerArrayLiteralIntoStorage()) - any other initializer shape
    /// (`let b = a`, copying one array-typed value into another) is
    /// deliberately kept unsupported (returns false) per M7B spec §13 -
    /// see this method's own .cpp comment for the full rationale.
    bool generateArrayVarDeclStmt(const ast::VarDeclStmt& varDecl, semantic::SymbolId id, llvm::ArrayType* arrayType,
                                   llvm::Function& function, llvm::IRBuilder<>& builder,
                                   const semantic::SemanticModel& model);

    /// KAI LANGUAGE M7B: stores `array`'s elements directly into
    /// `storage` (an `arrayType`-typed address), left to right, EXACTLY
    /// ONCE each - no separate aggregate-SSA-value construction step. A
    /// nested ArrayLiteralExpr element recurses into this SAME function
    /// against a GEP'd sub-address (see this method's own .cpp comment
    /// for why this is the one case M7B's "multidimensional arrays only
    /// if recursion genuinely works without broadening the
    /// implementation" allowance covers). Returns false the moment any
    /// element fails to lower or its LLVM type disagrees with the
    /// array's own element type - `storage` may be left partially
    /// initialized on failure, but the caller always discards it rather
    /// than ever reading through it.
    bool lowerArrayLiteralIntoStorage(const ast::ArrayLiteralExpr& array, llvm::Value* storage,
                                       llvm::ArrayType* arrayType, const semantic::SemanticModel& model,
                                       llvm::IRBuilder<>& builder);

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
                                                  const semantic::SemanticModel& model, llvm::IRBuilder<>& builder);
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

    /// KAI LANGUAGE M7B: the checked element ADDRESS `xs[index]` names -
    /// shared by both a plain read (lowerIndexExpr()) and an indexed
    /// write (lowerIndexAssignmentExpr()), so the bounds-check control
    /// flow is built exactly once, never duplicated between the two.
    /// Supports EXACTLY `xs[index]` where `xs` is a direct (through
    /// transparent ParenExpr only) identifier resolving to a
    /// SymbolKind::Local array binding - any other base (nested
    /// indexing, a call/member/parameter base) returns std::nullopt
    /// (M7B spec §1/§12: "generalized nested lvalue mutation," and
    /// multidimensional reads through it, remain deferred - the
    /// frontend's own checkIndexExpr() does not restrict this shape for
    /// a plain READ the way checkIndexAssignmentTarget() restricts a
    /// WRITE, so this is a real, if narrow, codegen-only limitation, not
    /// a frontend one).
    ///
    /// `index.index()` is evaluated EXACTLY ONCE (M7B spec §10/§11) and
    /// reused for both the bounds comparison and the GEP - never
    /// re-lowered. The comparison normalizes the index to an unsigned
    /// i64 (CreateSExtOrTrunc/CreateZExtOrTrunc, safe regardless of the
    /// index's own concrete width, including i64/u64 itself) and
    /// compares it against the array's own uint64 length - a signed
    /// index is ADDITIONALLY required to be non-negative at its OWN
    /// width before that normalization (M7B spec §6). On success,
    /// returns the GEP'd element address with `builder` positioned in
    /// the in-bounds successor block. On failure (out of bounds), emits
    /// `llvm.trap` + `unreachable` in the failure block (M7B spec §5) -
    /// NEVER KAI `panic`, no unwinding, no recovery - and the CALLER
    /// never receives a value in that case (this method itself does not
    /// return early there; it simply never reaches the point of
    /// returning the address, since the trap block is unreachable by
    /// construction). The element address is never computed, and no
    /// GEP/load/store against it ever happens, before the in-bounds
    /// branch succeeds.
    struct ArrayElementAddress {
        llvm::Value* pointer;
        llvm::Type* elementType;
    };
    std::optional<ArrayElementAddress> lowerArrayElementAddress(const ast::IndexExpr& indexExpr,
                                                                  const semantic::SemanticModel& model,
                                                                  llvm::IRBuilder<>& builder);

    /// `xs[index]` as a value-producing expression: the checked element
    /// address (lowerArrayElementAddress()) followed by one load.
    std::optional<llvm::Value*> lowerIndexExpr(const ast::IndexExpr& index, const semantic::SemanticModel& model,
                                                llvm::IRBuilder<>& builder);

    /// `xs[index] = value` for a mutable local array binding
    /// (dispatched to from lowerAssignmentExpr() when the target is an
    /// IndexExpr). The checked element address is computed first
    /// (lowerArrayElementAddress()); `value` is lowered - EXACTLY ONCE -
    /// only once that address is known to be in bounds (M7B spec §11:
    /// "store only after bounds success" - evaluating `value` before an
    /// out-of-bounds trap would run its side effects for no reason, on a
    /// path that can never continue anyway). Returns
    /// std::optional<llvm::Value*>{nullptr} - Unit success - on a
    /// successful store, matching lowerAssignmentExpr()'s own
    /// identifier-target convention exactly.
    std::optional<llvm::Value*> lowerIndexAssignmentExpr(const ast::IndexExpr& indexTarget, const ast::Expr& value,
                                                          const semantic::SemanticModel& model,
                                                          llvm::IRBuilder<>& builder);

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
    ///   Str               -> extract {ptr,len},   call `kai_print_str`
    ///   anything else (Char, ...)                 -> explicit failure
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
    ///
    /// KAI LANGUAGE M7B: Array now lowers to a real `llvm::ArrayType`
    /// (`[N x ElementType]`, via `model`'s own arrayElementType()/
    /// arrayLength() - never independently re-derived), driven entirely
    /// by whatever this SAME function can already lower for the element
    /// type - never a separate hardcoded "supported array element kinds"
    /// list. This automatically and correctly covers every
    /// already-lowerable scalar (all eight integer kinds, f32/f64, bool,
    /// str) and even a nested array, while Char (still nullptr
    /// standalone) correctly keeps `[char; N]` unsupported too, with no
    /// special-casing needed either way. `model` is required for this
    /// one case only - every primitive kind ignores it entirely, so any
    /// call site's own model is always a valid argument.
    llvm::Type* lowerType(semantic::Type type, const semantic::SemanticModel& model);

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

    /// See unsupportedConstruct()'s own doc comment. Sets the member only
    /// on the FIRST call per generate() (later/nested deferred-construct
    /// failures during the same generate() call never overwrite the
    /// first, most relevant one).
    void recordUnsupportedConstruct(std::string description, SourceSpan span);

    const SourceManager& sources_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    std::vector<std::pair<semantic::SymbolId, llvm::AllocaInst*>> locals_;
    std::vector<std::pair<semantic::SymbolId, llvm::Function*>> functions_;
    std::optional<UnsupportedConstruct> unsupportedConstruct_;
};

} // namespace kai::codegen
