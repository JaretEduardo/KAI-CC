#include "kai/semantic/TypeChecker.hpp"

#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/Symbol.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace kai::semantic {

namespace {

// Only a CONCRETE expected Type supplies contextual typing (Milestone 1
// spec #6): nullopt, Type::unresolved(), and Type::error() are all
// treated as "no usable expected-type context" for literal typing.
std::optional<Type> usableContext(std::optional<Type> expected) {
    if (!expected.has_value() || expected->isUnresolved() || expected->isError()) {
        return std::nullopt;
    }
    return expected;
}

// Only used by checkUnaryExpr()'s Negate special case. Walks through
// transparent ParenExpr wrappers to find the LiteralExpr this UnaryExpr
// ultimately negates, if any - nullptr for anything else (an identifier,
// a call, ...), so `-x` still falls through to the general
// (Type::unresolved()) UnaryExpr path. `chain` collects every ParenExpr
// transparently unwrapped along the way, so the caller can stamp the
// same contextual Type onto each of them too (Milestone 1 spec #13).
const ast::LiteralExpr* unwrapAdaptableLiteral(const ast::Expr& expr, std::vector<const ast::ParenExpr*>& chain) {
    if (expr.kind() == ast::ExprKind::Paren) {
        const auto& paren = static_cast<const ast::ParenExpr&>(expr);
        chain.push_back(&paren);
        return unwrapAdaptableLiteral(paren.inner(), chain);
    }
    if (expr.kind() == ast::ExprKind::Literal) {
        const auto& literal = static_cast<const ast::LiteralExpr&>(expr);
        if (literal.literalKind() == ast::LiteralKind::Integer || literal.literalKind() == ast::LiteralKind::Float) {
            return &literal;
        }
    }
    return nullptr;
}

// std::from_chars into a std::uint64_t magnitude (Milestone 1 spec #27) -
// the lexical digit magnitude, decoded BEFORE any sign is applied. The
// current lexer only ever produces plain decimal digit sequences for an
// Integer literal, so no base prefix / separator handling is needed.
std::optional<std::uint64_t> decodeIntegerMagnitude(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// The largest magnitude each concrete integer TypeKind can hold on its
// positive side, and (for signed kinds only) on its negative side - e.g.
// i8's positive magnitude limit is 127, its negative limit is 128 (for
// -128). Never derived from a signed int64_t conversion (Milestone 1 spec
// #13 explicitly forbids that path, since e.g. -2147483648's magnitude
// 2147483648 does not fit in a signed int32 at all).
struct IntegerRange {
    std::uint64_t maxPositiveMagnitude;
    std::uint64_t maxNegativeMagnitude; // meaningful only when isSigned
    bool isSigned;
};

// No `default:` case: TypeKind is fully implemented today, so -Wswitch
// fires the moment a new TypeKind is added without a case here.
std::optional<IntegerRange> integerRangeFor(Type type) {
    switch (type.kind()) {
        case TypeKind::I8:
            return IntegerRange{127ull, 128ull, true};
        case TypeKind::I16:
            return IntegerRange{32767ull, 32768ull, true};
        case TypeKind::I32:
            return IntegerRange{2147483647ull, 2147483648ull, true};
        case TypeKind::I64:
            return IntegerRange{9223372036854775807ull, 9223372036854775808ull, true};
        case TypeKind::U8:
            return IntegerRange{255ull, 0ull, false};
        case TypeKind::U16:
            return IntegerRange{65535ull, 0ull, false};
        case TypeKind::U32:
            return IntegerRange{4294967295ull, 0ull, false};
        case TypeKind::U64:
            return IntegerRange{std::numeric_limits<std::uint64_t>::max(), 0ull, false};
        case TypeKind::Unresolved:
        case TypeKind::Error:
        case TypeKind::Unit:
        case TypeKind::F32:
        case TypeKind::F64:
        case TypeKind::Bool:
        case TypeKind::Char:
        case TypeKind::Str:
            return std::nullopt;
    }
    return std::nullopt;
}

bool integerLiteralFits(Type target, bool negative, std::uint64_t magnitude) {
    const std::optional<IntegerRange> range = integerRangeFor(target);
    if (!range.has_value()) {
        return false;
    }
    if (negative) {
        return range->isSigned && magnitude <= range->maxNegativeMagnitude;
    }
    return magnitude <= range->maxPositiveMagnitude;
}

// Milestone 2 spec #9: a pure, structural, AST-shape-only predicate - no
// SemanticModel lookup, no checkExpr call. Answers "could this
// subexpression's OWN final Type still be freely chosen by an enclosing
// concrete numeric context" - true only for a numeric literal, transitively
// through ParenExpr, through UnaryExpr::Negate, and through an arithmetic
// BinaryExpr whose own operands are BOTH themselves flexible in this sense.
// Exhaustive over both ast::ExprKind and (nested) ast::BinaryOperator, no
// `default:`, mirroring every other dispatch switch in this file.
bool canAcceptNumericContext(const ast::Expr& expr) {
    switch (expr.kind()) {
        case ast::ExprKind::Literal: {
            const auto& literal = static_cast<const ast::LiteralExpr&>(expr);
            return literal.literalKind() == ast::LiteralKind::Integer ||
                   literal.literalKind() == ast::LiteralKind::Float;
        }

        case ast::ExprKind::Paren:
            return canAcceptNumericContext(static_cast<const ast::ParenExpr&>(expr).inner());

        case ast::ExprKind::Unary: {
            const auto& unary = static_cast<const ast::UnaryExpr&>(expr);
            return unary.op() == ast::UnaryOperator::Negate && canAcceptNumericContext(unary.operand());
        }

        case ast::ExprKind::Binary: {
            const auto& binary = static_cast<const ast::BinaryExpr&>(expr);
            switch (binary.op()) {
                case ast::BinaryOperator::Add:
                case ast::BinaryOperator::Subtract:
                case ast::BinaryOperator::Multiply:
                case ast::BinaryOperator::Divide:
                case ast::BinaryOperator::Modulo:
                    return canAcceptNumericContext(binary.left()) && canAcceptNumericContext(binary.right());
                case ast::BinaryOperator::Or:
                case ast::BinaryOperator::And:
                case ast::BinaryOperator::Equal:
                case ast::BinaryOperator::NotEqual:
                case ast::BinaryOperator::Less:
                case ast::BinaryOperator::LessEqual:
                case ast::BinaryOperator::Greater:
                case ast::BinaryOperator::GreaterEqual:
                case ast::BinaryOperator::Range:
                    // A comparison/equality/logical/Range result is never
                    // itself a flexible numeric value (Bool, or deferred
                    // Unresolved) - it can never be "the" numeric context an
                    // enclosing expression discovers through it.
                    return false;
            }
            return false; // unreachable, -Wreturn-type guard only
        }

        case ast::ExprKind::Identifier:
        case ast::ExprKind::Call:
        case ast::ExprKind::Assignment:
        case ast::ExprKind::ArrayLiteral:
        case ast::ExprKind::Index:
        case ast::ExprKind::Member:
        case ast::ExprKind::Unit:
        case ast::ExprKind::ErrorPropagation:
            return false;
    }
    return false; // unreachable, -Wreturn-type guard only
}

// Milestone 2 spec #10/#12: only arithmetic/modulo ever let the
// whole-expression `expected` flow into their operands (their successful
// result type IS the shared operand type) - and only when `expected` is
// itself usable (concrete) AND numeric. `expectedAnnotationSpan` travels
// alongside `expected` only in that same case, preserving provenance
// (spec #14) without needing a general context-provenance object.
std::pair<std::optional<Type>, std::optional<SourceSpan>> arithmeticOperandContext(
    std::optional<Type> expected, std::optional<SourceSpan> expectedAnnotationSpan) {
    const std::optional<Type> context = usableContext(expected);
    if (context.has_value() && context->isNumeric()) {
        return {context, expectedAnnotationSpan};
    }
    return {std::nullopt, std::nullopt};
}

bool isNumericDomain(Type type) { return type.isNumeric(); }
bool isIntegerDomain(Type type) { return type.isInteger(); }
bool isEqualityDomain(Type type) { return type.isNumeric() || type.isBool() || type.isChar(); }

// Milestone 3 spec #3: structural only, mirrors unwrapAdaptableLiteral's own
// shape - unwraps ONLY transparent ParenExpr wrappers on the way to a bare
// IdentifierExpr. Returns nullptr for anything else (a Binary/Call/Member/
// ...), so `(1 + 2)()`, `obj.member()`, `result?()`, and `foo()()` are never
// structurally rewritten into a direct function identifier - they fall
// through to the generic callee-classification path in checkCallExpr(),
// based on their own checked semantic Type. No first-class Function Type is
// introduced by this helper or by anything that consumes it.
const ast::IdentifierExpr* unwrapDirectCalleeIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapDirectCalleeIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

// Milestone 4 spec #3: structural only, mirrors unwrapDirectCalleeIdentifier's
// own shape - unwraps ONLY transparent ParenExpr wrappers on the way to a
// bare IdentifierExpr. Returns nullptr for anything else (a Binary/Call/
// Member/Index/...), so a non-identifier assignment target is never
// structurally rewritten - it is classified on its own shape in
// checkAssignmentExpr().
const ast::IdentifierExpr* unwrapAssignmentTargetIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapAssignmentTargetIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

// Spellable str + Parameters/Returns MVP (M9): structural-only Paren
// unwrap, mirroring unwrapAssignmentTargetIdentifier/
// unwrapDirectCalleeIdentifier above - used only by checkReturnStmt()'s
// temporary UnsupportedStrReturn rule to see through `return ("literal")`
// to the literal itself.
const ast::Expr& unwrapParenExpr(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapParenExpr(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    return expr;
}

} // namespace

TypeChecker::TypeChecker(const SourceManager& sources) noexcept : sources_(sources) {}

void TypeChecker::check(const ast::SourceFile& file, SemanticModel& model) {
    for (const auto& decl : file.declarations()) {
        checkTopLevelDeclaration(*decl, model);
    }
}

// No `default:` case: DeclKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's own exhaustive switch over it.
void TypeChecker::checkTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model) const {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            checkFunctionBody(static_cast<const ast::FunctionDecl&>(decl), model);
            return;
    }
}

void TypeChecker::checkFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model) const {
    // Milestone 5 spec #8: use the declaration mapping (each FunctionDecl -
    // including a duplicate - has its own SymbolId/signature), never a
    // textual name lookup, and never re-resolve the return TypeSyntax -
    // SemanticAnalyzer already did that in Pass 1.
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    // Copied by value immediately (Milestone 5 spec #8): TypeChecker adds
    // no symbols, but this avoids any lifetime coupling to SemanticModel's
    // internal Symbol storage regardless, and only the small Type value is
    // actually needed for the rest of this function's traversal. The
    // annotation span (spec #9) comes from the AST's own TypeSyntax*, not
    // from the Symbol - nullopt for an implicit Unit return.
    // Spellable str + Parameters/Returns MVP (M9): a pure signature-shape
    // count, computed once here from the already-resolved
    // FunctionSignature - never re-derived per return statement, and
    // meaningless when the return type isn't Str (checkReturnStmt() only
    // consults it in that case).
    std::size_t strParameterCount = 0;
    for (const Type paramType : model.symbol(*fnId).signature->parameterTypes) {
        strParameterCount += paramType.isStr() ? 1 : 0;
    }

    const ReturnContext returnContext{
        model.symbol(*fnId).signature->returnType,
        fn.returnType() != nullptr ? std::optional<SourceSpan>(fn.returnType()->span()) : std::nullopt,
        strParameterCount,
    };

    checkBlock(fn.body(), returnContext, model);
}

void TypeChecker::checkBlock(const ast::BlockStmt& block, const ReturnContext& returnContext,
                              SemanticModel& model) const {
    for (const auto& stmt : block.statements()) {
        checkStatement(*stmt, returnContext, model);
    }
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's own exhaustive switch over it.
void TypeChecker::checkStatement(const ast::Stmt& stmt, const ReturnContext& returnContext,
                                  SemanticModel& model) const {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            checkBlock(static_cast<const ast::BlockStmt&>(stmt), returnContext, model);
            return;

        case ast::StmtKind::Expr:
            inferExpr(static_cast<const ast::ExprStmt&>(stmt).expr(), model);
            return;

        case ast::StmtKind::VarDecl:
            checkVarDecl(static_cast<const ast::VarDeclStmt&>(stmt), model);
            return;

        case ast::StmtKind::Return:
            checkReturnStmt(static_cast<const ast::ReturnStmt&>(stmt), returnContext, model);
            return;

        case ast::StmtKind::If:
            checkIfStmt(static_cast<const ast::IfStmt&>(stmt), returnContext, model);
            return;

        case ast::StmtKind::While:
            checkWhileStmt(static_cast<const ast::WhileStmt&>(stmt), returnContext, model);
            return;

        case ast::StmtKind::For:
            checkForStmt(static_cast<const ast::ForStmt&>(stmt), returnContext, model);
            return;
    }
}

void TypeChecker::checkIfStmt(const ast::IfStmt& ifStmt, const ReturnContext& returnContext,
                               SemanticModel& model) const {
    // Milestone 5 spec #4: every condition is validated independently;
    // every branch body - including one whose own condition mismatched or
    // was Error/Unresolved - is still traversed unconditionally. `else`
    // has no condition of its own.
    for (const ast::IfBranch& branch : ifStmt.branches()) {
        checkCondition(*branch.condition, model);
        checkBlock(*branch.body, returnContext, model);
    }
    if (const std::optional<ast::ElseClause>& elseClause = ifStmt.elseClause(); elseClause.has_value()) {
        checkBlock(*elseClause->body, returnContext, model);
    }
}

void TypeChecker::checkWhileStmt(const ast::WhileStmt& whileStmt, const ReturnContext& returnContext,
                                  SemanticModel& model) const {
    checkCondition(whileStmt.condition(), model);
    checkBlock(whileStmt.body(), returnContext, model);
}

void TypeChecker::checkForStmt(const ast::ForStmt& forStmt, const ReturnContext& returnContext,
                                SemanticModel& model) const {
    // Milestone 5 spec #6: UNCHANGED from Milestone 1 - no iterable-type/
    // Range/element-type validation, and the for-variable's Symbol type is
    // untouched by this milestone.
    inferExpr(forStmt.iterable(), model);
    checkBlock(forStmt.body(), returnContext, model);
}

void TypeChecker::checkCondition(const ast::Expr& condition, SemanticModel& model) const {
    // Milestone 5 spec #2-#3: a CONCRETE expected Type (Bool) states the
    // semantic contract directly. Every current expression kind already
    // refuses to contextually adapt to Bool on its own (literals only
    // adapt within their own numeric family; arithmetic/modulo only
    // accept a numeric outer context; comparison/equality/logical/calls/
    // assignment never consult their own `expected` at all), so this is
    // observationally identical to inferExpr() + comparison today - no
    // condition-specific expression typing is introduced.
    const Type conditionType = checkExpr(condition, Type::boolean(), model);

    if (!conditionType.isError() && !conditionType.isUnresolved() && !(conditionType == Type::boolean())) {
        model.addError(SemanticError{
            SemanticErrorKind::TypeMismatch,
            condition.span(),
            std::nullopt,
            Type::boolean(),
            conditionType,
        });
    }
}

void TypeChecker::checkReturnStmt(const ast::ReturnStmt& returnStmt, const ReturnContext& returnContext,
                                   SemanticModel& model) const {
    const Type declaredReturnType = returnContext.returnType;

    if (declaredReturnType.isError() || declaredReturnType.isUnresolved()) {
        // Milestone 5 spec #13/#14: the declared return annotation itself
        // already failed/was deferred - check the returned expression (if
        // any) with no usable context so its own independent errors still
        // surface, but never compare it against Error/Unresolved.
        if (const ast::Expr* value = returnStmt.value(); value != nullptr) {
            inferExpr(*value, model);
        }
        return;
    }

    // Milestone 5 spec #15/#16: a bare `return` is treated as Type::unit()
    // for this comparison ONLY - no AST node is fabricated, and no
    // expression-type entry is ever recorded for it (ReturnStmt is a
    // statement, not an Expr). A concrete declared return type is
    // otherwise checked exactly like checkVarDecl() checks an initializer
    // against its annotation - Unit is not special-cased in any way.
    const Type actualType = returnStmt.value() != nullptr
                                 ? checkExpr(*returnStmt.value(), declaredReturnType, model, returnContext.annotationSpan)
                                 : Type::unit();

    if (!actualType.isError() && !actualType.isUnresolved() && !(actualType == declaredReturnType)) {
        model.addError(SemanticError{
            SemanticErrorKind::TypeMismatch,
            returnStmt.value() != nullptr ? returnStmt.value()->span() : returnStmt.span(),
            returnContext.annotationSpan,
            declaredReturnType,
            actualType,
        });
        return;
    }

    // Spellable str + Parameters/Returns MVP (M9): the type-check above
    // passed (actualType == declaredReturnType == Str). Every currently
    // constructible `str` value ultimately originates from static literal
    // storage (there is no String, no heap-backed owner, no borrowed-view
    // mechanism yet), so returning ANY well-typed `str` is safe today -
    // EXCEPT that once more than one `str` parameter exists, a
    // non-literal return's true provenance is ambiguous without a real
    // provenance analysis (which this milestone deliberately does not
    // build). Reject only that narrow, ambiguous shape; a literal return
    // is always allowed regardless of parameter count, since it can never
    // depend on any parameter's value.
    if (declaredReturnType.isStr() && returnContext.strParameterCount > 1 && returnStmt.value() != nullptr &&
        unwrapParenExpr(*returnStmt.value()).kind() != ast::ExprKind::Literal) {
        model.addError(SemanticError{
            SemanticErrorKind::UnsupportedStrReturn,
            returnStmt.value()->span(),
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }
}

void TypeChecker::checkVarDecl(const ast::VarDeclStmt& varDecl, SemanticModel& model) const {
    const std::optional<SymbolId> symbolId = model.declarationSymbol(varDecl.name());
    assert(symbolId.has_value());

    if (varDecl.type() == nullptr) {
        // Unannotated (Milestone 1 spec #20): infer from the initializer
        // with no usable expected context, then push the result onto the
        // Local Symbol SemanticAnalyzer already created (starting
        // Type::unresolved()).
        const Type inferred = inferExpr(varDecl.initializer(), model);
        model.setSymbolType(*symbolId, inferred);
        return;
    }

    const Type declaredType = model.symbol(*symbolId).type;

    if (declaredType.isError() || declaredType.isUnresolved()) {
        // The annotation itself already failed/was deferred during
        // SemanticAnalyzer's own declaration phase (Milestone 1 spec
        // #19): check the initializer with no usable expected context,
        // emit no TypeMismatch, and leave the Symbol's type exactly as
        // that phase set it.
        inferExpr(varDecl.initializer(), model);
        return;
    }

    const Type initializerType = checkExpr(varDecl.initializer(), declaredType, model, varDecl.type()->span());

    if (!initializerType.isError() && !initializerType.isUnresolved() && !(initializerType == declaredType)) {
        model.addError(SemanticError{
            SemanticErrorKind::TypeMismatch,
            varDecl.initializer().span(),
            varDecl.type()->span(),
            declaredType,
            initializerType,
        });
    }

    // The Symbol's declared type is never changed by a mismatched/failed
    // initializer (Milestone 1 spec #19) - it stays exactly as declared.
}

Type TypeChecker::checkExpr(const ast::Expr& expr, std::optional<Type> expected, SemanticModel& model,
                             std::optional<SourceSpan> expectedAnnotationSpan) const {
    switch (expr.kind()) {
        case ast::ExprKind::Literal:
            return checkLiteralExpr(static_cast<const ast::LiteralExpr&>(expr), expected, expectedAnnotationSpan,
                                     model);

        case ast::ExprKind::Unit: {
            const Type result = Type::unit();
            model.setExpressionType(expr, result);
            return result;
        }

        case ast::ExprKind::Identifier:
            return checkIdentifierExpr(static_cast<const ast::IdentifierExpr&>(expr), model);

        case ast::ExprKind::Paren:
            return checkParenExpr(static_cast<const ast::ParenExpr&>(expr), expected, expectedAnnotationSpan, model);

        case ast::ExprKind::Unary:
            return checkUnaryExpr(static_cast<const ast::UnaryExpr&>(expr), expected, expectedAnnotationSpan, model);

        case ast::ExprKind::Binary:
            return checkBinaryExpr(static_cast<const ast::BinaryExpr&>(expr), expected, expectedAnnotationSpan,
                                    model);

        case ast::ExprKind::Call:
            return checkCallExpr(static_cast<const ast::CallExpr&>(expr), model);

        case ast::ExprKind::Assignment:
            return checkAssignmentExpr(static_cast<const ast::AssignmentExpr&>(expr), model);

        case ast::ExprKind::ArrayLiteral:
            return checkArrayLiteralExpr(static_cast<const ast::ArrayLiteralExpr&>(expr), model);

        case ast::ExprKind::Index:
            return checkIndexExpr(static_cast<const ast::IndexExpr&>(expr), model);

        case ast::ExprKind::Member:
            return checkMemberExpr(static_cast<const ast::MemberExpr&>(expr), model);

        case ast::ExprKind::ErrorPropagation:
            return checkErrorPropagationExpr(static_cast<const ast::ErrorPropagationExpr&>(expr), model);
    }

    // Unreachable while ExprKind's enumerators match the switch above
    // exactly - kept only so -Wreturn-type doesn't warn (same idiom as
    // SemanticAnalyzer.cpp's resolveTypeSyntax()); the switch itself
    // still has no `default:`, so -Wswitch still fires the moment a new
    // ExprKind is added without a case here.
    return Type::error();
}

Type TypeChecker::inferExpr(const ast::Expr& expr, SemanticModel& model) const {
    return checkExpr(expr, std::nullopt, model);
}

Type TypeChecker::checkLiteralExpr(const ast::LiteralExpr& literal, std::optional<Type> expected,
                                    std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const {
    const std::optional<Type> context = usableContext(expected);
    Type result = Type::unresolved();

    switch (literal.literalKind()) {
        case ast::LiteralKind::Integer:
            result = checkIntegerLiteralValue(sources_.text(literal.span()), /*negative=*/false, literal.span(),
                                               context, expectedAnnotationSpan, model);
            break;

        case ast::LiteralKind::Float:
            // Integer literals never adapt to float, and symmetrically
            // float literals never adapt to integer targets (Milestone 1
            // spec #16) - only a concrete F32/F64 context changes
            // anything; every other context (including a concrete
            // integer type) falls back to the F64 default.
            result = (context.has_value() && context->isFloat()) ? *context : Type::f64();
            break;

        case ast::LiteralKind::Bool:
            result = Type::boolean();
            break;

        case ast::LiteralKind::Char:
            result = Type::character();
            break;

        case ast::LiteralKind::String:
            // Minimal String Literal Support milestone: a string literal
            // now has the concrete internal Type::str() (see its own
            // comment in Type.hpp) instead of Type::unresolved(). This is
            // deliberately narrow - it types the LITERAL expression only;
            // it does not make `str` a spellable type annotation (that
            // remains UnknownType via SemanticAnalyzer's
            // lookupPrimitiveTypeName(), unchanged) and does not model
            // any operator/method contract for Str beyond ordinary
            // existing domain checks (isNumericDomain/isEqualityDomain
            // below both already exclude Str, so e.g. `"a" + "b"` still
            // correctly fails as InvalidBinaryOperands).
            result = Type::str();
            break;
    }

    model.setExpressionType(literal, result);
    return result;
}

Type TypeChecker::checkIdentifierExpr(const ast::IdentifierExpr& identifier, SemanticModel& model) const {
    Type result = Type::error();

    if (const std::optional<SymbolId> id = model.resolution(identifier)) {
        const Symbol& symbol = model.symbol(*id);
        switch (symbol.kind) {
            case SymbolKind::Parameter:
            case SymbolKind::Local:
                result = symbol.type;
                break;
            case SymbolKind::Function:
            case SymbolKind::Builtin:
                // Outside call-specific semantics (not implemented this
                // milestone), a function/builtin name used as a plain
                // expression has no modeled Type yet (Milestone 1 spec
                // #9) - Unresolved, not a fabricated Function TypeKind.
                result = Type::unresolved();
                break;
        }
    }
    // No resolution at all: SemanticAnalyzer already emitted
    // UnknownIdentifier for this use (Milestone 1 spec #9) - Error, and
    // no new diagnostic here.

    model.setExpressionType(identifier, result);
    return result;
}

Type TypeChecker::checkParenExpr(const ast::ParenExpr& paren, std::optional<Type> expected,
                                  std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const {
    // Transparent (Milestone 1 spec #10): the SAME resulting Type is
    // recorded for both the inner expression and this ParenExpr, so e.g.
    // `let x: i64 = (10)` types both the literal and the ParenExpr as
    // i64.
    const Type result = checkExpr(paren.inner(), expected, model, expectedAnnotationSpan);
    model.setExpressionType(paren, result);
    return result;
}

Type TypeChecker::checkUnaryExpr(const ast::UnaryExpr& unary, std::optional<Type> expected,
                                  std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const {
    if (unary.op() == ast::UnaryOperator::Negate) {
        std::vector<const ast::ParenExpr*> parenChain;
        if (const ast::LiteralExpr* literal = unwrapAdaptableLiteral(unary.operand(), parenChain)) {
            // The complete negated literal - including any transparent
            // ParenExpr wrappers between it and this Negate - is treated
            // as ONE contextually typed numeric constant (Milestone 1
            // spec #13/#14): the same resulting Type is stamped onto the
            // literal, every wrapper, and this UnaryExpr itself.
            const std::optional<Type> context = usableContext(expected);
            Type result = Type::unresolved();

            if (literal->literalKind() == ast::LiteralKind::Integer) {
                result = checkIntegerLiteralValue(sources_.text(literal->span()), /*negative=*/true, unary.span(),
                                                   context, expectedAnnotationSpan, model);
            } else {
                result = (context.has_value() && context->isFloat()) ? *context : Type::f64();
            }

            model.setExpressionType(*literal, result);
            for (const ast::ParenExpr* wrapper : parenChain) {
                model.setExpressionType(*wrapper, result);
            }
            model.setExpressionType(unary, result);
            return result;
        }

        // General Negate (Milestone 2 spec #7): the operand is NOT a bare
        // literal reducible through transparent ParenExpr wrappers (an
        // identifier, a call, compound arithmetic, ...). `expected` (and
        // its annotation span) is forwarded into the operand ONLY when it
        // names a concrete type Negate's domain could actually produce
        // (signed integer or float) - forwarding an incompatible type
        // (unsigned/Bool/Char/Unit) would let a nested literal wrongly
        // adapt to a type Negate can never return, trading a more useful
        // TypeMismatch for a confusing InvalidUnaryOperand (see the
        // `let y: u8 = -(1 + 2)` example in the Milestone-2 design).
        // Everything else - fixed IdentifierExpr/CallExpr/etc. - ignores
        // whatever is forwarded anyway (unchanged Milestone-1 behavior),
        // so this forwarding is always safe.
        const std::optional<Type> context = usableContext(expected);
        const bool domainCompatible = context.has_value() && (context->isSignedInteger() || context->isFloat());
        const std::optional<Type> forwardedExpected = domainCompatible ? context : std::nullopt;
        const std::optional<SourceSpan> forwardedSpan = domainCompatible ? expectedAnnotationSpan : std::nullopt;

        const Type operandType = checkExpr(unary.operand(), forwardedExpected, model, forwardedSpan);

        Type result = Type::error();
        if (operandType.isError()) {
            result = Type::error();
        } else if (operandType.isUnresolved()) {
            result = Type::unresolved();
        } else if (operandType.isSignedInteger() || operandType.isFloat()) {
            result = operandType; // Negate is closed over its domain: type in = type out.
        } else {
            model.addError(SemanticError{
                SemanticErrorKind::InvalidUnaryOperand,
                unary.operatorSpan(),
                std::nullopt,
                std::nullopt,
                operandType,
            });
            result = Type::error();
        }

        model.setExpressionType(unary, result);
        return result;
    }

    if (unary.op() == ast::UnaryOperator::Not) {
        // Milestone 2 spec #8: the operand never receives contextual
        // forwarding - Bool is already context-immune (Milestone 1), and
        // no other type could ever become Bool through context. No
        // truthiness: a concrete non-Bool operand is always rejected.
        const Type operandType = inferExpr(unary.operand(), model);

        Type result = Type::error();
        if (operandType.isError()) {
            result = Type::error();
        } else if (operandType.isUnresolved()) {
            result = Type::unresolved();
        } else if (operandType.isBool()) {
            result = Type::boolean();
        } else {
            model.addError(SemanticError{
                SemanticErrorKind::InvalidUnaryOperand,
                unary.operatorSpan(),
                std::nullopt,
                std::nullopt,
                operandType,
            });
            result = Type::error();
        }

        model.setExpressionType(unary, result);
        return result;
    }

    // Ref/RefMut remain fully deferred (Milestone 2 spec #19/#23): the
    // operand is still fully checked (no expected context - reference
    // semantics are undefined), but the outer UnaryExpr stays
    // Type::unresolved() unconditionally, with no operator diagnostic.
    inferExpr(unary.operand(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(unary, result);
    return result;
}

Type TypeChecker::checkBinaryExpr(const ast::BinaryExpr& binary, std::optional<Type> expected,
                                   std::optional<SourceSpan> expectedAnnotationSpan, SemanticModel& model) const {
    switch (binary.op()) {
        case ast::BinaryOperator::Range: {
            // Still fully deferred (Milestone 2 spec #18/#23, unchanged
            // Milestone-1 rule): both endpoints are checked, but the outer
            // Range BinaryExpr stays Type::unresolved() unconditionally -
            // even when the endpoints' types plainly differ - and never
            // produces InvalidBinaryOperands.
            inferExpr(binary.left(), model);
            inferExpr(binary.right(), model);
            const Type result = Type::unresolved();
            model.setExpressionType(binary, result);
            return result;
        }

        case ast::BinaryOperator::Add:
        case ast::BinaryOperator::Subtract:
        case ast::BinaryOperator::Multiply:
        case ast::BinaryOperator::Divide: {
            const auto [operandExpected, operandSpan] = arithmeticOperandContext(expected, expectedAnnotationSpan);
            const auto [leftType, rightType] =
                checkMatchedOperands(binary.left(), binary.right(), operandExpected, operandSpan, model);
            const Type result = resolveMatchedOperatorResult(binary, leftType, rightType, isNumericDomain,
                                                               /*resultIsOperandType=*/true, model);
            model.setExpressionType(binary, result);
            return result;
        }

        case ast::BinaryOperator::Modulo: {
            const auto [operandExpected, operandSpan] = arithmeticOperandContext(expected, expectedAnnotationSpan);
            const auto [leftType, rightType] =
                checkMatchedOperands(binary.left(), binary.right(), operandExpected, operandSpan, model);
            const Type result = resolveMatchedOperatorResult(binary, leftType, rightType, isIntegerDomain,
                                                               /*resultIsOperandType=*/true, model);
            model.setExpressionType(binary, result);
            return result;
        }

        case ast::BinaryOperator::Less:
        case ast::BinaryOperator::LessEqual:
        case ast::BinaryOperator::Greater:
        case ast::BinaryOperator::GreaterEqual: {
            // Milestone 2 spec #10/#20: the whole-expression `expected`
            // NEVER flows into comparison operands - a comparison's
            // successful result is Bool, not the operand type - so
            // checkMatchedOperands() is called with no operand context of
            // its own; only sibling anchoring (inside that helper) still
            // applies.
            const auto [leftType, rightType] =
                checkMatchedOperands(binary.left(), binary.right(), std::nullopt, std::nullopt, model);
            const Type result = resolveMatchedOperatorResult(binary, leftType, rightType, isNumericDomain,
                                                               /*resultIsOperandType=*/false, model);
            model.setExpressionType(binary, result);
            return result;
        }

        case ast::BinaryOperator::Equal:
        case ast::BinaryOperator::NotEqual: {
            // Same rule as comparison (Milestone 2 spec #10/#21): no
            // whole-expression context, sibling anchoring only.
            const auto [leftType, rightType] =
                checkMatchedOperands(binary.left(), binary.right(), std::nullopt, std::nullopt, model);
            const Type result = resolveMatchedOperatorResult(binary, leftType, rightType, isEqualityDomain,
                                                               /*resultIsOperandType=*/false, model);
            model.setExpressionType(binary, result);
            return result;
        }

        case ast::BinaryOperator::And:
        case ast::BinaryOperator::Or: {
            // Logical operands never need checkMatchedOperands()
            // (Milestone 2 spec #12/#22): Bool is already context-immune,
            // so no anchor mechanism has anything to offer either side.
            const Type leftType = inferExpr(binary.left(), model);
            const Type rightType = inferExpr(binary.right(), model);

            Type result = Type::error();
            if (leftType.isError() || rightType.isError()) {
                result = Type::error();
            } else if (leftType.isUnresolved() || rightType.isUnresolved()) {
                result = Type::unresolved();
            } else if (leftType.isBool() && rightType.isBool()) {
                result = Type::boolean();
            } else {
                model.addError(SemanticError{
                    SemanticErrorKind::InvalidBinaryOperands,
                    binary.operatorSpan(),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                });
                result = Type::error();
            }

            model.setExpressionType(binary, result);
            return result;
        }
    }

    // Unreachable while BinaryOperator's enumerators match the switch
    // above exactly - kept only so -Wreturn-type doesn't warn; the switch
    // itself still has no `default:`.
    const Type result = Type::error();
    model.setExpressionType(binary, result);
    return result;
}

std::pair<Type, Type> TypeChecker::checkMatchedOperands(const ast::Expr& left, const ast::Expr& right,
                                                          std::optional<Type> operandExpected,
                                                          std::optional<SourceSpan> operandExpectedAnnotationSpan,
                                                          SemanticModel& model) const {
    const bool leftFlexible = canAcceptNumericContext(left);
    const bool rightFlexible = canAcceptNumericContext(right);

    if (leftFlexible && !rightFlexible) {
        // Discover the anchor from the fixed right side FIRST - it is
        // checked exactly once, with no context of its own, and its
        // result (if concrete numeric) becomes the context offered to the
        // flexible left side. No annotation span travels with a
        // sibling-derived anchor (Milestone 2 spec #14).
        const Type rightType = checkExpr(right, std::nullopt, model);
        const std::optional<Type> anchor = rightType.isNumeric() ? std::optional<Type>(rightType) : std::nullopt;
        const Type leftType = checkExpr(left, anchor, model);
        return {leftType, rightType};
    }

    if (!leftFlexible && rightFlexible) {
        const Type leftType = checkExpr(left, std::nullopt, model);
        const std::optional<Type> anchor = leftType.isNumeric() ? std::optional<Type>(leftType) : std::nullopt;
        const Type rightType = checkExpr(right, anchor, model);
        return {leftType, rightType};
    }

    if (leftFlexible && rightFlexible) {
        // No internal anchor exists - only the caller-supplied
        // whole-expression context (already filtered per operator family
        // by the caller) can inform either side, and it is offered to
        // both identically.
        const Type leftType = checkExpr(left, operandExpected, model, operandExpectedAnnotationSpan);
        const Type rightType = checkExpr(right, operandExpected, model, operandExpectedAnnotationSpan);
        return {leftType, rightType};
    }

    // Neither side is flexible - context never applies to either.
    const Type leftType = checkExpr(left, std::nullopt, model);
    const Type rightType = checkExpr(right, std::nullopt, model);
    return {leftType, rightType};
}

Type TypeChecker::resolveMatchedOperatorResult(const ast::BinaryExpr& binary, Type leftType, Type rightType,
                                                bool (*domainAccepts)(Type), bool resultIsOperandType,
                                                SemanticModel& model) const {
    if (leftType.isError() || rightType.isError()) {
        return Type::error();
    }
    if (leftType.isUnresolved() || rightType.isUnresolved()) {
        return Type::unresolved();
    }
    if (leftType == rightType && domainAccepts(leftType)) {
        return resultIsOperandType ? leftType : Type::boolean();
    }

    model.addError(SemanticError{
        SemanticErrorKind::InvalidBinaryOperands,
        binary.operatorSpan(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    });
    return Type::error();
}

Type TypeChecker::checkCallExpr(const ast::CallExpr& call, SemanticModel& model) const {
    // Milestone 3 spec #2: classification is driven ONLY by
    // SemanticModel::resolution()/SymbolKind - never by typeOf(callee)
    // (a Function/Builtin IdentifierExpr intentionally stays Unresolved,
    // spec #4) and never by identifier source text (spec #21).
    if (const ast::IdentifierExpr* directIdentifier = unwrapDirectCalleeIdentifier(call.callee())) {
        if (const std::optional<SymbolId> id = model.resolution(*directIdentifier)) {
            const Symbol& symbol = model.symbol(*id);
            if (symbol.kind == SymbolKind::Function) {
                // Records Unresolved through the identifier and every
                // transparent ParenExpr wrapper (spec #4/#22) - the exact
                // same, unmodified checkIdentifierExpr()/checkParenExpr()
                // logic every other identifier/paren use already goes
                // through.
                inferExpr(call.callee(), model);
                return checkUserFunctionCall(call, symbol, model);
            }
            if (symbol.kind == SymbolKind::Builtin) {
                inferExpr(call.callee(), model);
                return checkBuiltinCall(call, model);
            }
            // Parameter/Local: not a function/builtin - fall through to
            // the generic path below, classified by its OWN Type.
        }
        // Unresolved identifier (UnknownIdentifier already emitted by
        // SemanticAnalyzer): also falls through to the generic path.
    }

    const Type calleeType = inferExpr(call.callee(), model);
    for (const auto& argument : call.arguments()) {
        inferExpr(*argument, model);
    }

    Type result = Type::error();
    if (calleeType.isError()) {
        // Spec #18: e.g. an unresolved callee identifier - no second
        // "not callable" diagnosis on top of the already-reported problem.
        result = Type::error();
    } else if (calleeType.isUnresolved()) {
        // Spec #17/#19: a non-function-typed Local/Parameter whose own
        // type isn't modeled yet, or a deferred Member/Index/
        // ErrorPropagation/Call/other non-identifier callee expression -
        // don't guess either way.
        result = Type::unresolved();
    } else {
        // Spec #17: a genuinely concrete, non-function-typed callee.
        model.addError(SemanticError{
            SemanticErrorKind::NotCallable,
            call.callee().span(),
            std::nullopt,
            std::nullopt,
            calleeType,
        });
        result = Type::error();
    }

    model.setExpressionType(call, result);
    return result;
}

Type TypeChecker::checkBuiltinCall(const ast::CallExpr& call, SemanticModel& model) const {
    // Spec #20: builtin CALL semantics remain deferred - every argument
    // is still checked (for its own independent expression errors, and so
    // every visited node gets a typeOf entry), with no expected context,
    // no argument-count check, and no argument-type check. CallExpr is
    // unconditionally Type::unresolved(), even when a child argument
    // itself becomes Error - unlike a validated user Function call (spec
    // #10), a Builtin call's own "contract" is not yet modeled at all, so
    // there is nothing concrete for a child Error to invalidate.
    for (const auto& argument : call.arguments()) {
        inferExpr(*argument, model);
    }
    const Type result = Type::unresolved();
    model.setExpressionType(call, result);
    return result;
}

Type TypeChecker::checkUserFunctionCall(const ast::CallExpr& call, const Symbol& functionSymbol,
                                         SemanticModel& model) const {
    // Spec #5: every SymbolKind::Function Symbol SemanticAnalyzer creates
    // has a signature - an existing semantic-model invariant. TypeChecker
    // adds no symbols and mutates no scopes; asserting here (rather than
    // fabricating an Unresolved signature) matches SemanticAnalyzer.cpp's
    // own established style for this exact invariant.
    assert(functionSymbol.signature.has_value());
    const FunctionSignature& signature = *functionSymbol.signature;

    const std::size_t paramCount = signature.parameterTypes.size();
    const std::size_t argCount = call.arguments().size();
    const std::size_t sharedCount = std::min(paramCount, argCount);

    bool hasConcreteMismatch = false;
    bool hasArgumentError = false;

    for (std::size_t i = 0; i < sharedCount; ++i) {
        const ast::Expr& argument = *call.arguments()[i];
        const Type paramType = signature.parameterTypes[i];
        const std::optional<Type> paramContext = usableContext(paramType);

        // No expectedAnnotationSpan (spec #6): FunctionSignature retains
        // no parameter TypeSyntax span provenance.
        const Type argumentType = checkExpr(argument, paramContext, model);

        if (argumentType.isError()) {
            // Spec #10: no TypeMismatch on top of an already-Error
            // argument - just remember the call is not fully valid.
            hasArgumentError = true;
            continue;
        }
        if (argumentType.isUnresolved() || !paramContext.has_value()) {
            // Spec #11/#12: argument compatibility deferred, not proven
            // invalid - emit nothing, and do not treat this position as a
            // reason to reject the call outright.
            continue;
        }
        if (!(argumentType == paramType)) {
            hasConcreteMismatch = true;
            model.addError(SemanticError{
                SemanticErrorKind::TypeMismatch,
                argument.span(),
                std::nullopt,
                paramType,
                argumentType,
            });
        }
    }

    // Extra arguments (spec #15): still checked, with no expected type.
    for (std::size_t i = sharedCount; i < argCount; ++i) {
        inferExpr(*call.arguments()[i], model);
    }

    Type result = Type::error();
    if (argCount != paramCount) {
        // Spec #15/#16: emitted AFTER every syntactically-present
        // argument has already been visited above - independent argument
        // diagnostics (TypeMismatch, or a pre-existing UnknownIdentifier)
        // are never suppressed by a wrong count.
        model.addError(SemanticError{
            SemanticErrorKind::InvalidArgumentCount,
            call.span(),
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
        result = Type::error();
    } else if (hasConcreteMismatch || hasArgumentError) {
        // Spec #9/#10: a genuinely-known-invalid call.
        result = Type::error();
    } else {
        // Spec #11/#12/#13: nothing CONCRETELY wrong was found - deferred
        // (Unresolved/Error) parameter/argument positions do not erase an
        // otherwise-known declared return type.
        const Type returnType = signature.returnType;
        if (returnType.isError()) {
            result = Type::error(); // no new diagnostic - UnknownType already belongs to the declaration
        } else if (returnType.isUnresolved()) {
            result = Type::unresolved();
        } else {
            result = returnType; // spec #14: never adapted to any OUTER expected context
        }
    }

    model.setExpressionType(call, result);
    return result;
}

Type TypeChecker::checkAssignmentExpr(const ast::AssignmentExpr& assignment, SemanticModel& model) const {
    // Milestone 4 spec #3: classification is driven ONLY by
    // SemanticModel::resolution()/SymbolKind - never by identifier source
    // text, and never by typeOf(target) (a Function/Builtin identifier
    // intentionally stays Unresolved, mirroring Milestone 3's callee rule).
    if (const ast::IdentifierExpr* directTarget = unwrapAssignmentTargetIdentifier(assignment.target())) {
        if (const std::optional<SymbolId> id = model.resolution(*directTarget)) {
            const Symbol& symbol = model.symbol(*id);
            switch (symbol.kind) {
                case SymbolKind::Local:
                case SymbolKind::Parameter:
                    return checkVariableAssignmentTarget(assignment, symbol, model);
                case SymbolKind::Function:
                case SymbolKind::Builtin:
                    return checkInvalidAssignmentTarget(assignment, model);
            }
        }

        // Unresolved identifier target (Milestone 4 spec #8):
        // UnknownIdentifier was already emitted by SemanticAnalyzer - no
        // new diagnostic, no context for the RHS.
        inferExpr(assignment.target(), model);
        inferExpr(assignment.value(), model);
        const Type result = Type::error();
        model.setExpressionType(assignment, result);
        return result;
    }

    if (assignment.target().kind() == ast::ExprKind::Member || assignment.target().kind() == ast::ExprKind::Index) {
        return checkDeferredAssignmentTarget(assignment, model);
    }

    // Categorically invalid target shape (Milestone 4 spec #9): a
    // literal, Unit, a general unary/binary expression, a call, an array
    // literal, error propagation, a nested assignment, or anything else
    // that isn't a direct identifier and isn't Member/Index.
    return checkInvalidAssignmentTarget(assignment, model);
}

Type TypeChecker::checkVariableAssignmentTarget(const ast::AssignmentExpr& assignment, const Symbol& symbol,
                                                 SemanticModel& model) const {
    const ast::Expr& target = assignment.target();
    const ast::Expr& value = assignment.value();

    // Records typeOf entries through the identifier and every transparent
    // ParenExpr wrapper via the existing, unmodified checkIdentifierExpr()/
    // checkParenExpr() logic (Milestone 4 spec #18).
    const Type targetType = inferExpr(target, model);

    if (!symbol.isMutable) {
        // Milestone 4 spec #5: the RHS is still checked (independent
        // errors surface), but with NO target-type context, and NO
        // TypeMismatch is attempted - mutability alone already
        // disqualifies this assignment.
        inferExpr(value, model);
        model.addError(SemanticError{
            SemanticErrorKind::AssignmentToImmutableBinding,
            target.span(),
            symbol.declaredAt,
            std::nullopt,
            std::nullopt,
        });
        const Type result = Type::error();
        model.setExpressionType(assignment, result);
        return result;
    }

    if (targetType.isError()) {
        // Milestone 4 spec #16 (the correction): Error is deliberately
        // NOT treated like Unresolved - the target's declared type is
        // already known to be genuinely broken (e.g. an UnknownType
        // annotation), so this assignment is unconditionally Error
        // regardless of what the RHS turns out to be, stopping any
        // downstream cascade exactly like an already-Error operand would.
        inferExpr(value, model);
        const Type result = Type::error();
        model.setExpressionType(assignment, result);
        return result;
    }

    if (targetType.isUnresolved()) {
        // Milestone 4 spec #15: compatibility is deferred, but
        // AssignmentExpr's own result (Unit) is independently known
        // regardless - unless the RHS itself is a genuine Error.
        const Type valueType = inferExpr(value, model);
        const Type result = valueType.isError() ? Type::error() : Type::unit();
        model.setExpressionType(assignment, result);
        return result;
    }

    // Concrete target type (Milestone 4 spec #11-#14): the RHS is checked
    // contextually against it, reusing checkExpr() exactly as
    // checkVarDecl() already does - no assignment-specific literal/
    // expression-context algorithm.
    const Type valueType = checkExpr(value, targetType, model);

    Type result = Type::unit();
    if (valueType.isError()) {
        result = Type::error();
    } else if (!valueType.isUnresolved() && !(valueType == targetType)) {
        model.addError(SemanticError{
            SemanticErrorKind::TypeMismatch,
            value.span(),
            std::nullopt,
            targetType,
            valueType,
        });
        result = Type::error();
    }

    model.setExpressionType(assignment, result);
    return result;
}

Type TypeChecker::checkInvalidAssignmentTarget(const ast::AssignmentExpr& assignment, SemanticModel& model) const {
    inferExpr(assignment.target(), model);
    inferExpr(assignment.value(), model);
    model.addError(SemanticError{
        SemanticErrorKind::InvalidAssignmentTarget,
        assignment.target().span(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    });
    const Type result = Type::error();
    model.setExpressionType(assignment, result);
    return result;
}

Type TypeChecker::checkDeferredAssignmentTarget(const ast::AssignmentExpr& assignment, SemanticModel& model) const {
    inferExpr(assignment.target(), model);
    inferExpr(assignment.value(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(assignment, result);
    return result;
}

Type TypeChecker::checkArrayLiteralExpr(const ast::ArrayLiteralExpr& array, SemanticModel& model) const {
    for (const auto& element : array.elements()) {
        inferExpr(*element, model);
    }
    const Type result = Type::unresolved();
    model.setExpressionType(array, result);
    return result;
}

Type TypeChecker::checkIndexExpr(const ast::IndexExpr& index, SemanticModel& model) const {
    inferExpr(index.object(), model);
    inferExpr(index.index(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(index, result);
    return result;
}

Type TypeChecker::checkMemberExpr(const ast::MemberExpr& member, SemanticModel& model) const {
    // Only the object is checked - `member` is syntactic metadata whose
    // resolution depends on the object's (not-yet-modeled) type, exactly
    // mirroring SemanticAnalyzer's own MemberExpr handling.
    inferExpr(member.object(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(member, result);
    return result;
}

Type TypeChecker::checkErrorPropagationExpr(const ast::ErrorPropagationExpr& errorPropagation,
                                             SemanticModel& model) const {
    inferExpr(errorPropagation.operand(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(errorPropagation, result);
    return result;
}

Type TypeChecker::checkIntegerLiteralValue(std::string_view text, bool negative, SourceSpan diagnosticSpan,
                                            std::optional<Type> context,
                                            std::optional<SourceSpan> expectedAnnotationSpan,
                                            SemanticModel& model) const {
    // A concrete non-integer context (e.g. f64) supplies no contextual
    // target here - the default target is always I32 unless `context`
    // itself names a concrete integer type (Milestone 1 spec #11/#12).
    Type target = Type::i32();
    if (context.has_value() && context->isInteger()) {
        target = *context;
    }

    const std::optional<std::uint64_t> magnitude = decodeIntegerMagnitude(text);
    if (!magnitude.has_value() || !integerLiteralFits(target, negative, *magnitude)) {
        model.addError(SemanticError{
            SemanticErrorKind::LiteralOutOfRange,
            diagnosticSpan,
            expectedAnnotationSpan,
            target,
            std::nullopt,
        });
        return Type::error();
    }

    return target;
}

} // namespace kai::semantic
