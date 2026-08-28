#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Symbol.hpp"
#include "kai/source/SourceLocation.hpp"
#include "kai/source/SourceManager.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace kai::semantic {

/// TYPE-CHECK MILESTONE 1+2+3+4+5: Literal & Annotation Foundation,
/// Primitive Operators, Function Calls, Assignment & Binding Mutability,
/// and Conditions & Return Validation.
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
/// Milestone 2 adds primitive-operator typing: Negate, Not, arithmetic
/// (+ - * /), modulo (%), ordering (< <= > >=), equality (== !=), and
/// logical (&& ||) - see checkUnaryExpr()/checkBinaryExpr() in
/// TypeChecker.cpp.
///
/// Milestone 3 adds function-call typing for calls whose callee resolves
/// (through transparent ParenExpr wrappers only) to a user-declared
/// SymbolKind::Function - argument-count validation, positional
/// contextual argument type checking against FunctionSignature, and
/// CallExpr result typing from FunctionSignature.returnType - see
/// checkCallExpr()/checkUserFunctionCall() in TypeChecker.cpp. Builtin
/// calls remain intentionally fully deferred (their signatures are not
/// committed); every other callee shape (a non-function-typed Local/
/// Parameter, a literal, a deferred Member/Index/ErrorPropagation/Call
/// expression, ...) is classified generically from its own checked Type.
///
/// Milestone 4 adds assignment typing for a target that resolves (through
/// transparent ParenExpr wrappers only) to a SymbolKind::Local or
/// SymbolKind::Parameter - mutability checking, RHS contextual typing
/// against the target's Type, RHS-vs-target TypeMismatch, and
/// AssignmentExpr's own result (Type::unit() on success) - see
/// checkAssignmentExpr()/checkVariableAssignmentTarget() in
/// TypeChecker.cpp. A MemberExpr/IndexExpr target remains intentionally
/// deferred (Type::unresolved(), no diagnostic - mutation through those
/// forms is not yet modeled); every other target shape (a literal, a
/// call, a Function/Builtin identifier, ...) is a categorically invalid
/// target.
///
/// Milestone 5 adds if/while condition Bool validation and return-
/// statement checking against the enclosing FunctionDecl's declared
/// return Type (see ReturnContext, checkCondition(), checkIfStmt()/
/// checkWhileStmt()/checkReturnStmt() in TypeChecker.cpp). A bare
/// `return` is checked as though it returned Type::unit() - no AST node
/// is fabricated for this; it is a purely local value used only for the
/// comparison. This milestone still does NOT implement all-paths-return
/// analysis, missing-return-at-function-end diagnostics, unreachable-code
/// analysis, or for-iterable validation - `ForStmt::iterable()` is still
/// only traversed for its own independent errors, never type-checked.
///
/// Reference/range/borrow-mutation semantics remain deferred - every such
/// outer expression kind is still fully traversed (its children are
/// checked) but recorded as Type::unresolved() (see checkIndexExpr()/etc.
/// in TypeChecker.cpp for the full per-ExprKind rules).
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

    /// Milestone 5: the enclosing FunctionDecl's declared return Type
    /// (already resolved by SemanticAnalyzer - never re-resolved here)
    /// plus the source span of its explicit `-> T` annotation, if any
    /// (nullopt for an implicit Unit return). Created exactly once per
    /// FunctionDecl, in checkFunctionBody(), and threaded by const
    /// reference through the rest of that function's statement traversal
    /// - never stored in SemanticModel, never held as TypeChecker member
    /// state, so TypeChecker remains reentrant across functions/files.
    struct ReturnContext {
        Type returnType;
        std::optional<SourceSpan> annotationSpan;

        /// Spellable str + Parameters/Returns MVP (M9): the number of
        /// `str`-typed parameters the enclosing function declares.
        /// Consulted ONLY by checkReturnStmt()'s temporary
        /// UnsupportedStrReturn rule (see its own comment) - a narrow,
        /// signature-shape fact, never a general provenance/lifetime
        /// analysis. Meaningless (and unused) when returnType is not Str.
        std::size_t strParameterCount = 0;
    };

    void checkTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model) const;
    void checkFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model) const;
    void checkBlock(const ast::BlockStmt& block, const ReturnContext& returnContext, SemanticModel& model) const;

    /// Exhaustive over ast::StmtKind, no `default:`. Still does NOT
    /// implement mutability/assignment-target checking at the statement
    /// level beyond what checkVarDecl()/checkAssignmentExpr() already do,
    /// nor all-paths-return/unreachable-code analysis (Milestone 5 spec
    /// #23/#24) - only the ReturnStmt/IfStmt/WhileStmt cases below are new
    /// in this milestone.
    void checkStatement(const ast::Stmt& stmt, const ReturnContext& returnContext, SemanticModel& model) const;

    /// Implements the annotated-local and unannotated-local algorithms
    /// (Milestone 1 spec #19/#20): fetches the Local Symbol ALREADY
    /// created by SemanticAnalyzer through declarationSymbol() - never
    /// re-resolves the TypeSyntax annotation itself.
    void checkVarDecl(const ast::VarDeclStmt& varDecl, SemanticModel& model) const;

    /// Milestone 5 spec #2-#4: validates every if/else-if condition
    /// independently (never the parameterless `else`), then traverses
    /// every branch body - including a mismatched/Error/Unresolved
    /// condition's own body - unconditionally.
    void checkIfStmt(const ast::IfStmt& ifStmt, const ReturnContext& returnContext, SemanticModel& model) const;

    /// Milestone 5 spec #5: the same Bool-condition rule as checkIfStmt(),
    /// with no additional loop-specific validation; the body is always
    /// traversed afterward.
    void checkWhileStmt(const ast::WhileStmt& whileStmt, const ReturnContext& returnContext,
                         SemanticModel& model) const;

    /// KAI LANGUAGE M6 (`for` + integer ranges): the only supported
    /// iterable form is a literal `start..end` range (an existing
    /// BinaryExpr{Range} - see checkIntegerRangeFor()); anything else is
    /// rejected with SemanticErrorKind::UnsupportedForIterable rather
    /// than silently left Unresolved (M6 spec #2). The loop variable's
    /// Symbol - already declared by SemanticAnalyzer::analyzeForStmt()
    /// as an immutable Local with Type::unresolved() - has its real
    /// type pushed via model.setSymbolType() here, exactly like
    /// checkVarDecl() does for an unannotated `let`. The body is always
    /// checked afterward regardless of the iterable's own outcome (same
    /// "keep traversing" policy as every other statement in this file).
    void checkForStmt(const ast::ForStmt& forStmt, const ReturnContext& returnContext, SemanticModel& model) const;

    /// M6: validates `range`'s two endpoints via the SAME
    /// checkMatchedOperands()/resolveMatchedOperatorResult() machinery
    /// arithmetic operators already use - sibling-anchored contextual
    /// literal adaptation (e.g. `0` in `for i in 0..n` with `n: u32`
    /// adapts to u32 exactly like `0 + n` would), no new implicit-
    /// conversion system. Restricted to isIntegerDomain (floats/bool/
    /// char/str/unit rejected via the existing InvalidBinaryOperands
    /// path). Returns the matched element Type (or Error/Unresolved,
    /// following the same propagation rule as every other binary
    /// operator) - the CALLER (checkForStmt) is responsible for pushing
    /// it onto the loop variable's Symbol. `range`'s own whole-
    /// expression type is left exactly as checkBinaryExpr()'s general
    /// Range case already records it (Type::unresolved(),
    /// unconditionally) - a range is never itself a first-class runtime
    /// value in M6, only its endpoints and the loop variable are.
    Type checkIntegerRangeFor(const ast::BinaryExpr& range, SemanticModel& model) const;

    /// Milestone 5 spec #2-#3: `checkExpr(condition, Type::boolean(),
    /// model)` - a CONCRETE expected Type states the semantic contract
    /// directly, though every current expression kind already refuses to
    /// contextually adapt to Bool (an integer/float literal only adapts
    /// to a matching numeric family; arithmetic/modulo only accept a
    /// numeric outer context; comparison/equality/logical/calls/
    /// assignment never consult their own `expected` at all) - so this is
    /// observationally identical to inferExpr() followed by a comparison
    /// today, while stating the contract explicitly and staying
    /// consistent with every other "known expected type" call site.
    /// Emits TypeMismatch only when the resulting Type is concrete and
    /// not Bool; Error/Unresolved conditions emit nothing (spec #2).
    void checkCondition(const ast::Expr& condition, SemanticModel& model) const;

    /// Milestone 5 spec #7-#21: checks a return value (if present)
    /// contextually against `returnContext.returnType` exactly like
    /// checkVarDecl() checks an initializer against a declared
    /// annotation - no return-specific literal/expression-context
    /// algorithm. A bare `return` is treated as Type::unit() for this
    /// comparison ONLY (spec #15) - no AST node is fabricated and no
    /// expression-type entry is recorded for it, since ReturnStmt is a
    /// statement. Error/Unresolved on either side of the comparison never
    /// produces a diagnostic (spec #13/#14/#18/#19); a concrete mismatch
    /// reuses TypeMismatch, with `relatedSpan = returnContext.annotationSpan`
    /// (spec #9).
    ///
    /// Spellable str + Parameters/Returns MVP (M9): when the declared
    /// return type is Str and the check above passed, ALSO applies one
    /// narrow, temporary restriction: a non-literal `str` return (i.e.
    /// anything other than a string literal expression) is rejected with
    /// UnsupportedStrReturn when the enclosing function has more than one
    /// `str` parameter (`returnContext.strParameterCount > 1`). This is a
    /// pure signature-shape + one-expression-kind check - never dataflow/
    /// provenance tracking of WHICH parameter a value came from - kept
    /// only until a real return-provenance analysis exists (see
    /// MEMORY_MODEL.md §25's Static/ExternallyOwned/LocallyOwned design,
    /// intentionally NOT implemented here).
    void checkReturnStmt(const ast::ReturnStmt& returnStmt, const ReturnContext& returnContext,
                          SemanticModel& model) const;

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

    /// Dispatches first to the Milestone-1 bare-literal-through-Paren
    /// Negate fast path (unchanged - preserves the -128/-129/etc.
    /// boundary-safe behavior), then to the Milestone-2 general Negate/
    /// Not algorithms, then to the still-fully-deferred Ref/RefMut case
    /// (Milestone 2 spec #6-#8/#23).
    Type checkUnaryExpr(const ast::UnaryExpr& unary, std::optional<Type> expected,
                         std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const;

    /// Milestone 2: dispatches per ast::BinaryOperator family - Range
    /// stays fully deferred (Milestone 1 rule, unchanged); arithmetic/
    /// modulo/ordering/equality route through checkMatchedOperands() and
    /// resolveMatchedOperatorResult(); logical (&&/||) is checked inline,
    /// needing no operand-anchoring. `expected`/`expectedAnnotationSpan`
    /// are only ever honored by the arithmetic/modulo families (Milestone
    /// 2 spec #10/#12) - ordering/equality never let the whole-expression
    /// expected type flow into their operands.
    Type checkBinaryExpr(const ast::BinaryExpr& binary, std::optional<Type> expected,
                          std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const;

    /// The Milestone-2 operand-anchoring algorithm (spec #9/#12/#13),
    /// shared by arithmetic/modulo/ordering/equality checking. Each of
    /// `left`/`right` is checked through checkExpr() EXACTLY ONCE:
    /// - if exactly one side is canAcceptNumericContext() ("flexible")
    ///   and the other is not, the fixed side is checked first (with no
    ///   context) and its own concrete numeric Type - if any - becomes
    ///   the anchor context offered to the flexible side (never carrying
    ///   `operandExpectedAnnotationSpan`, since that anchor did not come
    ///   from an explicit annotation);
    /// - if both sides are flexible, `operandExpected`/
    ///   `operandExpectedAnnotationSpan` (the caller-filtered
    ///   whole-expression context, if any) is offered to both;
    /// - otherwise both sides are simply checked with no context.
    /// No subtree is ever re-checked.
    std::pair<Type, Type> checkMatchedOperands(const ast::Expr& left, const ast::Expr& right,
                                                std::optional<Type> operandExpected,
                                                std::optional<SourceSpan> operandExpectedAnnotationSpan,
                                                SemanticModel& model) const;

    /// Applies the Milestone-2 spec #4 Error/Unresolved propagation rule,
    /// then - only once both operands are concrete - the operator's own
    /// domain rule (`domainAccepts`, e.g. isNumeric()/isInteger()/a
    /// combined equality predicate) and same-type requirement.
    /// `resultIsOperandType` selects arithmetic/modulo's "result = shared
    /// operand type" vs. ordering/equality's "result = Bool". Emits
    /// InvalidBinaryOperands at `binary.operatorSpan()` on domain
    /// failure - never when either operand was already Error/Unresolved.
    Type resolveMatchedOperatorResult(const ast::BinaryExpr& binary, Type leftType, Type rightType,
                                       bool (*domainAccepts)(Type), bool resultIsOperandType,
                                       SemanticModel& model) const;

    /// Milestone 3 classification entry point. Classifies `call.callee()`
    /// (through transparent ParenExpr wrappers only, via
    /// unwrapDirectCalleeIdentifier() in TypeChecker.cpp) into: a direct
    /// Function-resolving identifier (checkUserFunctionCall()); a direct
    /// Builtin-resolving identifier (checkBuiltinCall()); or - for every
    /// other callee shape (a non-function-typed Local/Parameter, a
    /// literal, a deferred Member/Index/ErrorPropagation/Call expression,
    /// an unresolved identifier, ...) - a generic path that checks the
    /// callee with no expected context and classifies purely from its
    /// resulting Type: Error stays Error, Unresolved stays Unresolved
    /// (neither ever emits NotCallable), and any other concrete Type
    /// emits NotCallable and becomes Error. Classification never inspects
    /// identifier source text - only SemanticModel::resolution()/
    /// SymbolKind decide it - so a user function shadowing a Builtin
    /// (Milestone 3 spec #21) is handled correctly with no special-casing.
    Type checkCallExpr(const ast::CallExpr& call, SemanticModel& model) const;

    /// A resolved SymbolKind::Builtin callee (spec #20): the callee and
    /// every argument are still checked (for their own independent
    /// expression errors and so every visited node gets a typeOf entry),
    /// but with no expected context anywhere, no argument-count check, no
    /// argument-type check, and no NotCallable - CallExpr is always
    /// Type::unresolved(), even when a child argument is itself Error,
    /// because builtin CALL semantics (not the arguments' own expression
    /// semantics) are what remain deferred - builtin signatures are not
    /// committed (STANDARD_LIBRARY.md).
    Type checkBuiltinCall(const ast::CallExpr& call, SemanticModel& model) const;

    /// A resolved SymbolKind::Function callee (spec #6-#16): validates
    /// argument count, checks each shared-prefix argument against its
    /// corresponding parameter type (contextually, reusing checkExpr()
    /// exactly as checkVarDecl() does - no call-specific literal/
    /// expression-context algorithm), and computes CallExpr's result per
    /// the Milestone 3 recovery table: Error on a wrong argument count or
    /// any concrete argument TypeMismatch/argument Error; otherwise the
    /// declared FunctionSignature::returnType, filtered through the
    /// standard Error/Unresolved passthrough (an Unresolved argument or
    /// an Unresolved/Error parameter type defers ONLY that position's own
    /// compatibility check - it does not itself erase an otherwise-known
    /// concrete return type). `functionSymbol.signature` is asserted
    /// present (spec #5's SemanticAnalyzer-enforced invariant), never
    /// fabricated.
    Type checkUserFunctionCall(const ast::CallExpr& call, const Symbol& functionSymbol, SemanticModel& model) const;

    /// Milestone 4 classification entry point. Classifies
    /// `assignment.target()` (through transparent ParenExpr wrappers
    /// only, via unwrapAssignmentTargetIdentifier() in TypeChecker.cpp)
    /// into: a direct identifier resolving to Local/Parameter
    /// (checkVariableAssignmentTarget()); a direct identifier resolving
    /// to Function/Builtin, or any other categorically invalid target
    /// shape (checkInvalidAssignmentTarget()); a MemberExpr/IndexExpr
    /// target (checkDeferredAssignmentTarget()); or an unresolved
    /// identifier target (handled inline - no new diagnostic, spec #8).
    /// Classification never inspects identifier source text - only
    /// SemanticModel::resolution()/SymbolKind decide it.
    Type checkAssignmentExpr(const ast::AssignmentExpr& assignment, SemanticModel& model) const;

    /// A target resolving to SymbolKind::Local or SymbolKind::Parameter
    /// (spec #4-#5, #11-#17): checks mutability first (an immutable
    /// binding short-circuits straight to AssignmentToImmutableBinding,
    /// with the RHS still checked but with no target-type context), then
    /// - for a mutable binding - dispatches on the target's own recorded
    /// Type: Error unconditionally becomes Error (spec #16's correction -
    /// deliberately NOT treated like Unresolved, to stop a downstream
    /// cascade); Unresolved becomes Unit unless the RHS itself is Error;
    /// a concrete target Type contextualizes the RHS exactly like
    /// checkVarDecl() does, comparing via the existing TypeMismatch
    /// shape.
    Type checkVariableAssignmentTarget(const ast::AssignmentExpr& assignment, const Symbol& symbol,
                                        SemanticModel& model) const;

    /// Shared by two spec cases: a target identifier resolving to
    /// Function/Builtin (spec #7), and any categorically invalid target
    /// shape (spec #9) - both check the target and RHS (each exactly
    /// once, no context), then emit InvalidAssignmentTarget and return
    /// Type::error().
    Type checkInvalidAssignmentTarget(const ast::AssignmentExpr& assignment, SemanticModel& model) const;

    /// A MemberExpr/IndexExpr target (spec #10): checks the target
    /// through its own existing, unmodified checkMemberExpr()/
    /// checkIndexExpr() traversal and the RHS with no context, and
    /// always returns Type::unresolved() with no diagnostic - even when a
    /// child or the RHS is itself Error - because this assignment FORM
    /// remains intentionally deferred, mirroring the Range/Ref/RefMut/
    /// Builtin-call "deferred construct" rule rather than the "Error
    /// child propagates" rule implemented constructs use.
    Type checkDeferredAssignmentTarget(const ast::AssignmentExpr& assignment, SemanticModel& model) const;
    /// KAI LANGUAGE M7A: produces a real fixed-size array Type
    /// `[ElementType; N]` for a non-empty, homogeneous literal - reusing
    /// the SAME sibling/contextual-literal-adaptation spirit
    /// checkMatchedOperands() already uses for binary operators,
    /// generalized to N elements (an explicit `expected` array element
    /// type always wins; otherwise the first non-"flexible" element's
    /// own no-context type becomes the anchor offered to the rest; if
    /// every element is flexible, each simply defaults independently -
    /// still coherent, since a flexible literal's default never depends
    /// on position). Error/Unresolved propagate exactly like
    /// resolveMatchedOperatorResult() does. An inhomogeneous literal
    /// (e.g. `[1, true, 3]`) is rejected via
    /// SemanticErrorKind::IncompatibleArrayElementType. An empty literal
    /// `[]` is accepted ONLY when `expected` supplies a concrete array
    /// element type (M7A spec #10 - "no standalone inferred element
    /// type"); otherwise SemanticErrorKind::AmbiguousEmptyArrayLiteral.
    /// Never checks any element more than once (each element is checked
    /// EXACTLY once, whether or not it becomes the anchor). No length
    /// mismatch against `expected` is special-cased here at all: the
    /// literal's OWN structural type (element type + actual element
    /// count) is always what's returned, and the CALLER's existing
    /// generic `initializerType == declaredType` comparison
    /// (checkVarDecl()) already rejects a length mismatch correctly, for
    /// free, now that `[i32; 3] != [i32; 4]` is real Type inequality -
    /// no new length-specific diagnostic is invented here.
    Type checkArrayLiteralExpr(const ast::ArrayLiteralExpr& array, std::optional<Type> expected,
                                SemanticModel& model) const;

    /// KAI LANGUAGE M7A spec §12: deliberately UNCHANGED - `object[index]`
    /// element-read typing (index-domain validation, bounds checking)
    /// remains fully deferred to M7B. Still only checks `object`/`index`
    /// for their own independent errors and always returns
    /// Type::unresolved(), exactly as before Array became a real Type.
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
