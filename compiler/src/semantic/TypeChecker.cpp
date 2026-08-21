#include "kai/semantic/TypeChecker.hpp"

#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/Symbol.hpp"

#include <cassert>
#include <charconv>
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
    checkBlock(fn.body(), model);
}

void TypeChecker::checkBlock(const ast::BlockStmt& block, SemanticModel& model) const {
    for (const auto& stmt : block.statements()) {
        checkStatement(*stmt, model);
    }
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's own exhaustive switch over it. No statement
// validation happens yet - see TypeChecker.hpp's class comment.
void TypeChecker::checkStatement(const ast::Stmt& stmt, SemanticModel& model) const {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            checkBlock(static_cast<const ast::BlockStmt&>(stmt), model);
            return;

        case ast::StmtKind::Expr:
            inferExpr(static_cast<const ast::ExprStmt&>(stmt).expr(), model);
            return;

        case ast::StmtKind::VarDecl:
            checkVarDecl(static_cast<const ast::VarDeclStmt&>(stmt), model);
            return;

        case ast::StmtKind::Return: {
            const auto& returnStmt = static_cast<const ast::ReturnStmt&>(stmt);
            if (const ast::Expr* value = returnStmt.value(); value != nullptr) {
                inferExpr(*value, model);
            }
            return;
        }

        case ast::StmtKind::If: {
            const auto& ifStmt = static_cast<const ast::IfStmt&>(stmt);
            for (const ast::IfBranch& branch : ifStmt.branches()) {
                inferExpr(*branch.condition, model);
                checkBlock(*branch.body, model);
            }
            if (const std::optional<ast::ElseClause>& elseClause = ifStmt.elseClause(); elseClause.has_value()) {
                checkBlock(*elseClause->body, model);
            }
            return;
        }

        case ast::StmtKind::While: {
            const auto& whileStmt = static_cast<const ast::WhileStmt&>(stmt);
            inferExpr(whileStmt.condition(), model);
            checkBlock(whileStmt.body(), model);
            return;
        }

        case ast::StmtKind::For: {
            const auto& forStmt = static_cast<const ast::ForStmt&>(stmt);
            inferExpr(forStmt.iterable(), model);
            checkBlock(forStmt.body(), model);
            return;
        }
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
            return checkBinaryExpr(static_cast<const ast::BinaryExpr&>(expr), model);

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
            // Type model does not represent str/String yet (Milestone 1
            // spec #18) - Unresolved, never Error, and never a
            // TypeMismatch solely because of this temporary gap.
            result = Type::unresolved();
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
    }

    // General case (Milestone 1 spec #8): -identifier, !expr, &expr,
    // &mut expr, and Negate over anything that is not an
    // (optionally-parenthesized) adaptable numeric literal all remain
    // Type::unresolved() this milestone - but the operand is still fully
    // checked with no expected context.
    inferExpr(unary.operand(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(unary, result);
    return result;
}

Type TypeChecker::checkBinaryExpr(const ast::BinaryExpr& binary, SemanticModel& model) const {
    // Deferred (Milestone 1 spec #8/#21): both children are checked with
    // no expected context; the outer BinaryExpr stays Type::unresolved()
    // even if a child came back Type::error() - a child's error is
    // deliberately NOT propagated to this deferred outer node yet
    // (general binary typing, where it WILL propagate, is Milestone 2).
    inferExpr(binary.left(), model);
    inferExpr(binary.right(), model);
    const Type result = Type::unresolved();
    model.setExpressionType(binary, result);
    return result;
}

Type TypeChecker::checkCallExpr(const ast::CallExpr& call, SemanticModel& model) const {
    inferExpr(call.callee(), model);
    for (const auto& argument : call.arguments()) {
        inferExpr(*argument, model);
    }
    const Type result = Type::unresolved();
    model.setExpressionType(call, result);
    return result;
}

Type TypeChecker::checkAssignmentExpr(const ast::AssignmentExpr& assignment, SemanticModel& model) const {
    // Type::unresolved() FOR THIS MILESTONE only (Milestone 1 spec #8):
    // the committed future rule is assignment expression type = Unit,
    // but assignment semantics are not implemented yet.
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
