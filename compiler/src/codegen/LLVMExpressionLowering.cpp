// Expression lowering: lowerExpr() and everything it dispatches to
// (literals, identifiers, unary/binary/logical short-circuit, assignment,
// calls). Split out of LLVMCodeGenerator.cpp (M4 spec §25) once that file
// passed ~700 lines - a purely mechanical split: every method here is a
// private member of kai::codegen::LLVMCodeGenerator, declared once in
// LLVMCodeGenerator.hpp; nothing was made public to enable this split.
// Module/function orchestration and statement/block lowering remain in
// LLVMCodeGenerator.cpp.

#include "kai/codegen/LLVMCodeGenerator.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/Casting.h>

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kai::codegen {

using semantic::FunctionSignature;
using semantic::SemanticModel;
using semantic::Symbol;
using semantic::SymbolId;
using semantic::SymbolKind;
using semantic::Type;

namespace {

// Structural-only unwrap (never a semantic decision - TypeChecker's own
// unwrapAssignmentTargetIdentifier() in TypeChecker.cpp already validated
// this exact target shape): finds the bare IdentifierExpr an assignment
// target names, transparently through ParenExpr wrappers. Returns nullptr
// for any other shape (Member/Index/etc.) - those never reach a slot
// lookup, and fail there like any other unsupported target.
const ast::IdentifierExpr* unwrapAssignmentTargetIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapAssignmentTargetIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

// Structural-only unwrap, mirroring TypeChecker's own
// unwrapDirectCalleeIdentifier() in TypeChecker.cpp: finds the bare
// IdentifierExpr a call's callee names, transparently through ParenExpr
// wrappers. Returns nullptr for any other callee shape (a Binary/Call/
// Member/... callee) - those are unsupported call forms in M4 and fail at
// the lowerCallExpr() call site like any other unsupported case.
const ast::IdentifierExpr* unwrapDirectCalleeIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapDirectCalleeIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

// KAI LANGUAGE M10B: structural-only unwrap, mirroring TypeChecker's own
// unwrapSliceSourceIdentifier() in TypeChecker.cpp exactly - `slice(x)`'s
// eligible-source shape has already been fully validated there; this is
// the SAME structural check repeated here only because codegen needs the
// identifier itself (to resolve its SymbolId/storage), never a second,
// independent decision about whether it is eligible.
const ast::IdentifierExpr* unwrapSliceSourceIdentifier(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapSliceSourceIdentifier(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::Identifier) {
        return &static_cast<const ast::IdentifierExpr&>(expr);
    }
    return nullptr;
}

// The ONE narrow, structurally-identified exception to lowerExpr()'s
// Unresolved gate (M6 spec §8): true only for a CallExpr whose direct
// callee resolves - via `model.resolution()`/SymbolKind, never identifier
// text - to a Builtin named `print`. TypeChecker's checkBuiltinCall()
// always records Type::unresolved() for a builtin CALL itself (see
// TypeChecker.cpp), so without this a valid, supported `print(x)` could
// never reach lowerCallExpr() at all. Deliberately NOT "any Call is
// exempt" - an unresolved identifier, a non-Builtin/non-Function callee,
// or a recognized-but-unsupported Builtin (`panic`/`assert`) all still
// return false here and stay rejected by the ordinary Unresolved gate.
bool isPrintBuiltinCall(const ast::CallExpr& call, const SemanticModel& model) {
    const ast::IdentifierExpr* callee = unwrapDirectCalleeIdentifier(call.callee());
    if (callee == nullptr) {
        return false;
    }
    const std::optional<SymbolId> id = model.resolution(*callee);
    if (!id.has_value()) {
        return false;
    }
    const Symbol& symbol = model.symbol(*id);
    return symbol.kind == SymbolKind::Builtin && symbol.name == "print";
}

// Decodes a KAI string literal's exact byte content from its full source
// lexeme (`"..."`, quotes included - LiteralExpr never stores a decoded
// value; see its own class comment). This is the SAME kind of purely
// mechanical, already-validated-by-the-lexer decode as the integer/float/
// bool cases in lowerLiteralExpr() below: Lexer.cpp's scanString() has
// already rejected any literal whose escapes are not one of
// \n \r \t \\ \" \0 (GRAMMAR.md's String production), so this only
// translates those exact six escapes to their actual bytes - it never
// re-validates, and never recognizes any other escape.
//
// The result is a byte buffer, not a C string: std::string tolerates
// embedded '\0' bytes fine (its size()/data() never depend on strlen()),
// which matters because \0 is one of the supported escapes (M8 spec #6 -
// see kai_runtime.h's kai_print_str()).
std::string decodeStringLiteralBytes(std::string_view lexeme) {
    assert(lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"');
    std::string decoded;
    decoded.reserve(lexeme.size() - 2);

    for (std::size_t i = 1; i + 1 < lexeme.size(); ++i) {
        const char c = lexeme[i];
        if (c != '\\') {
            decoded.push_back(c);
            continue;
        }

        // The lexer only ever produces a StringLiteral token when a
        // backslash is followed by one more, in-bounds, supported escape
        // character - safe to advance and switch on it unconditionally.
        ++i;
        switch (lexeme[i]) {
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            case '\\':
                decoded.push_back('\\');
                break;
            case '"':
                decoded.push_back('"');
                break;
            case '0':
                decoded.push_back('\0');
                break;
            default:
                // Unreachable: Lexer::scanString()'s
                // isSupportedStringEscapeChar() already rejected every
                // other escape as Invalid before this token could ever
                // become a StringLiteral.
                assert(false && "unsupported string escape reached codegen");
                break;
        }
    }

    return decoded;
}

} // namespace

// No `default:` case: ExprKind is fully implemented today, mirroring the
// rest of this codebase's exhaustive-switch convention.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerExpr(const ast::Expr& expr, const SemanticModel& model,
                                                          llvm::IRBuilder<>& builder) {
    const std::optional<Type> type = model.typeOf(expr);
    if (!type.has_value() || type->isError()) {
        return std::nullopt;
    }
    if (type->isUnresolved()) {
        // See isPrintBuiltinCall()'s own comment: the ONE narrow,
        // structurally-identified exception to rejecting Unresolved here.
        if (expr.kind() != ast::ExprKind::Call ||
            !isPrintBuiltinCall(static_cast<const ast::CallExpr&>(expr), model)) {
            return std::nullopt;
        }
        return lowerCallExpr(static_cast<const ast::CallExpr&>(expr), *type, model, builder);
    }

    switch (expr.kind()) {
        case ast::ExprKind::Literal:
            return lowerLiteralExpr(static_cast<const ast::LiteralExpr&>(expr), *type, model, builder);
        case ast::ExprKind::Paren:
            return lowerExpr(static_cast<const ast::ParenExpr&>(expr).inner(), model, builder);
        case ast::ExprKind::Unary:
            return lowerUnaryExpr(static_cast<const ast::UnaryExpr&>(expr), model, builder);
        case ast::ExprKind::Binary:
            return lowerBinaryExpr(static_cast<const ast::BinaryExpr&>(expr), model, builder);
        case ast::ExprKind::Identifier:
            return lowerIdentifierExpr(static_cast<const ast::IdentifierExpr&>(expr), model, builder);
        case ast::ExprKind::Assignment:
            return lowerAssignmentExpr(static_cast<const ast::AssignmentExpr&>(expr), model, builder);
        case ast::ExprKind::Call:
            return lowerCallExpr(static_cast<const ast::CallExpr&>(expr), *type, model, builder);
        case ast::ExprKind::Index:
            // KAI LANGUAGE M7B: a real, checked element read - see
            // lowerIndexExpr()/lowerArrayElementAddress()'s own doc
            // comments for the full bounds-check/GEP/load design.
            return lowerIndexExpr(static_cast<const ast::IndexExpr&>(expr), model, builder);
        case ast::ExprKind::ArrayLiteral:
            // KAI LANGUAGE M8B: ArrayLiteralExpr is now a genuine value-
            // producing expression (`f([1, 2, 3])`, `return [1, 2, 3]`,
            // `a = [1, 2, 3]`), not only a VarDecl's own direct
            // initializer - see lowerArrayLiteralExpr()'s own doc comment
            // for the temp-storage-then-load strategy.
            return lowerArrayLiteralExpr(static_cast<const ast::ArrayLiteralExpr&>(expr), *type, model, builder);

        // Explicitly deferred: Member needs compound/struct types; Unit
        // is a legitimate value this milestone simply does not produce
        // as an expression's own literal form yet; error propagation
        // needs Result.
        case ast::ExprKind::Member:
        case ast::ExprKind::Unit:
        case ast::ExprKind::ErrorPropagation:
            return std::nullopt;
    }
    return std::nullopt;
}

// No `default:` case: ast::LiteralKind is fully implemented today.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerLiteralExpr(const ast::LiteralExpr& literal, Type type,
                                                                 const SemanticModel& model, llvm::IRBuilder<>&) {
    llvm::Type* llvmType = lowerType(type, model);
    if (llvmType == nullptr) {
        return std::nullopt;
    }

    const std::string_view text = sources_.text(literal.span());

    switch (literal.literalKind()) {
        case ast::LiteralKind::Integer: {
            if (!type.isInteger()) {
                return std::nullopt;
            }
            // LiteralExpr never decodes its own value (see its class
            // comment) - recover the literal's source text and decode it
            // exactly the way TypeChecker.cpp's decodeIntegerMagnitude()
            // already does. This is the SAME mechanical decimal-text ->
            // magnitude conversion, not a second semantic algorithm:
            // TypeChecker has already established that this text is a
            // valid, in-range literal of `type` - codegen never
            // re-derives range validity or contextual typing here, it
            // only re-reads the digits TypeChecker already validated.
            std::uint64_t magnitude = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), magnitude);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                return std::nullopt;
            }
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(llvmType), magnitude,
                                           type.isSignedInteger());
        }

        case ast::LiteralKind::Float: {
            if (!type.isFloat()) {
                return std::nullopt;
            }
            double value = 0.0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                return std::nullopt;
            }
            return llvm::ConstantFP::get(llvmType, value);
        }

        case ast::LiteralKind::Bool: {
            if (!type.isBool()) {
                return std::nullopt;
            }
            // GRAMMAR.md's boolean_literal is exactly "true" | "false" -
            // reading the literal's own spelling back is the same kind of
            // mechanical text read as the integer/float cases above, not
            // a second boolean-parsing algorithm.
            return llvm::ConstantInt::getBool(llvmType->getContext(), text == "true");
        }

        case ast::LiteralKind::String: {
            if (!type.isStr()) {
                return std::nullopt;
            }

            // Decode once, then build a private, internal-linkage,
            // read-only global holding the EXACT decoded bytes (no
            // heap allocation, no runtime ownership, no destructor - see
            // Type::str()'s own comment). AddNull=false: the descriptor's
            // length always comes from `decoded.size()` below, never from
            // any trailing terminator a helper might add, so an embedded
            // \0 byte (a supported escape) can never be confused with
            // string termination.
            const std::string decoded = decodeStringLiteralBytes(text);

            llvm::Constant* dataConstant =
                decoded.empty()
                    ? static_cast<llvm::Constant*>(
                          llvm::ConstantAggregateZero::get(llvm::ArrayType::get(llvm::Type::getInt8Ty(context_), 0)))
                    : static_cast<llvm::Constant*>(llvm::ConstantDataArray::getString(
                          context_, llvm::StringRef(decoded.data(), decoded.size()), /*AddNull=*/false));

            auto* global = new llvm::GlobalVariable(*module_, dataConstant->getType(), /*isConstant=*/true,
                                                      llvm::GlobalValue::PrivateLinkage, dataConstant, "kai.str");
            global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

            llvm::Constant* length = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), decoded.size());
            return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvmType), {global, length});
        }

        case ast::LiteralKind::Char:
            // Explicitly deferred - not part of this milestone's scope.
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerUnaryExpr(const ast::UnaryExpr& unary, const SemanticModel& model,
                                                               llvm::IRBuilder<>& builder) {
    // Ref/RefMut remain fully deferred - reference semantics do not exist
    // yet (see Type.hpp/TypeChecker.cpp: both stay Type::unresolved()).
    if (unary.op() != ast::UnaryOperator::Negate && unary.op() != ast::UnaryOperator::Not) {
        return std::nullopt;
    }

    const std::optional<llvm::Value*> operand = lowerExpr(unary.operand(), model, builder);
    if (!operand.has_value() || *operand == nullptr) {
        return std::nullopt;
    }

    // Semantic ground truth from TypeChecker - never re-inferred here.
    // TypeChecker's Negate rule is "type in = type out" and Not always
    // requires/produces Bool (see checkUnaryExpr() in TypeChecker.cpp),
    // so the operand's own recorded Type is exactly what selects the
    // correct LLVM instruction for either operator. Consumes whatever
    // Value lowerExpr() produced - a literal, a load, or (as of M4) a
    // CallInst - identically; no call-specific operator logic exists.
    const std::optional<Type> operandType = model.typeOf(unary.operand());
    if (!operandType.has_value()) {
        return std::nullopt;
    }

    if (unary.op() == ast::UnaryOperator::Negate) {
        // TypeChecker already rejects unsigned/Bool/Char/Unit operands for
        // Negate (InvalidUnaryOperand) - a successfully checked program
        // can only reach here with a signed integer or float operand.
        if (operandType->isSignedInteger()) {
            return builder.CreateNeg(*operand);
        }
        if (operandType->isFloat()) {
            return builder.CreateFNeg(*operand);
        }
        return std::nullopt;
    }

    // Not
    if (!operandType->isBool()) {
        return std::nullopt;
    }
    return builder.CreateNot(*operand);
}

// No `default:` case: ast::BinaryOperator is fully implemented today.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerBinaryExpr(const ast::BinaryExpr& binary,
                                                                const SemanticModel& model,
                                                                llvm::IRBuilder<>& builder) {
    if (binary.op() == ast::BinaryOperator::Range) {
        // Range semantic typing is still deferred (TypeChecker always
        // records Type::unresolved() for it) - lowerExpr()'s own
        // Unresolved gate would already catch this, but the explicit
        // check documents the reason directly at the lowering site.
        return std::nullopt;
    }

    // Short-circuit (M4 FINAL semantics): the right operand must NOT be
    // lowered unconditionally - handled entirely separately, before
    // either operand is touched, precisely because it must not follow
    // the eager "lower both operands first" shape below.
    if (binary.op() == ast::BinaryOperator::And || binary.op() == ast::BinaryOperator::Or) {
        return lowerLogicalExpr(binary, model, builder);
    }

    const std::optional<llvm::Value*> left = lowerExpr(binary.left(), model, builder);
    if (!left.has_value() || *left == nullptr) {
        return std::nullopt;
    }
    const std::optional<llvm::Value*> right = lowerExpr(binary.right(), model, builder);
    if (!right.has_value() || *right == nullptr) {
        return std::nullopt;
    }

    // TypeChecker requires `leftType == rightType` for every one of these
    // operators to type-check at all (see resolveMatchedOperatorResult()
    // in TypeChecker.cpp) - so the left operand's own recorded Type is
    // always sufficient, by itself, to select the correct LLVM
    // instruction/predicate. Never re-derived independently.
    const std::optional<Type> operandType = model.typeOf(binary.left());
    if (!operandType.has_value()) {
        return std::nullopt;
    }

    switch (binary.op()) {
        case ast::BinaryOperator::Add:
            if (operandType->isInteger()) {
                return builder.CreateAdd(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFAdd(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Subtract:
            if (operandType->isInteger()) {
                return builder.CreateSub(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFSub(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Multiply:
            if (operandType->isInteger()) {
                return builder.CreateMul(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFMul(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Divide:
            if (operandType->isSignedInteger()) {
                return builder.CreateSDiv(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateUDiv(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFDiv(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Modulo:
            // TypeChecker restricts Modulo to isIntegerDomain (see
            // checkBinaryExpr()'s Modulo case in TypeChecker.cpp) - float
            // operands never type-check for `%`, so no float Modulo
            // instruction selection is needed here.
            if (operandType->isSignedInteger()) {
                return builder.CreateSRem(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateURem(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Less:
            if (operandType->isSignedInteger()) {
                return builder.CreateICmpSLT(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateICmpULT(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpOLT(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::LessEqual:
            if (operandType->isSignedInteger()) {
                return builder.CreateICmpSLE(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateICmpULE(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpOLE(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Greater:
            if (operandType->isSignedInteger()) {
                return builder.CreateICmpSGT(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateICmpUGT(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpOGT(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::GreaterEqual:
            if (operandType->isSignedInteger()) {
                return builder.CreateICmpSGE(*left, *right);
            }
            if (operandType->isUnsignedInteger()) {
                return builder.CreateICmpUGE(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpOGE(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::Equal:
            // TypeChecker's isEqualityDomain also accepts Char - but no
            // ExprKind this milestone lowers can ever produce a Char
            // value (Char literals are explicitly deferred in
            // lowerLiteralExpr(), and identifiers/calls can only be
            // Local/Parameter/user-function results, never a Char-typed
            // source), so a Char operandType can never actually reach
            // here with both operands already lowered - the isInteger()/
            // isBool()/isFloat() checks below are exhaustive in practice,
            // and Char falls through to the explicit failure like any
            // other unsupported case.
            if (operandType->isInteger() || operandType->isBool()) {
                return builder.CreateICmpEQ(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpOEQ(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::NotEqual:
            if (operandType->isInteger() || operandType->isBool()) {
                return builder.CreateICmpNE(*left, *right);
            }
            if (operandType->isFloat()) {
                return builder.CreateFCmpONE(*left, *right);
            }
            return std::nullopt;

        case ast::BinaryOperator::And:
        case ast::BinaryOperator::Or:
            return std::nullopt; // handled above; unreachable

        case ast::BinaryOperator::Range:
            return std::nullopt; // handled above; unreachable
    }
    return std::nullopt;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerLogicalExpr(const ast::BinaryExpr& binary,
                                                                 const SemanticModel& model,
                                                                 llvm::IRBuilder<>& builder) {
    const bool isAnd = binary.op() == ast::BinaryOperator::And;

    const std::optional<llvm::Value*> lhs = lowerExpr(binary.left(), model, builder);
    if (!lhs.has_value() || *lhs == nullptr) {
        return std::nullopt;
    }

    // Read AFTER lowering lhs, never the block that was current before -
    // lhs may itself be a nested short-circuit expression that already
    // moved the builder (e.g. the `b || c` in `(b || c) && d`).
    llvm::BasicBlock* lhsEndBlock = builder.GetInsertBlock();
    llvm::Function* function = lhsEndBlock->getParent();

    llvm::BasicBlock* rhsBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.rhs" : "or.rhs", function);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.end" : "or.end", function);

    if (isAnd) {
        // lhs true -> must still evaluate rhs; lhs false -> short-circuit
        // to merge with `false`, never evaluating rhs.
        builder.CreateCondBr(*lhs, rhsBlock, mergeBlock);
    } else {
        // lhs true -> short-circuit to merge with `true`, never
        // evaluating rhs; lhs false -> must still evaluate rhs.
        builder.CreateCondBr(*lhs, mergeBlock, rhsBlock);
    }

    builder.SetInsertPoint(rhsBlock);
    const std::optional<llvm::Value*> rhs = lowerExpr(binary.right(), model, builder);
    if (!rhs.has_value() || *rhs == nullptr) {
        return std::nullopt;
    }
    // Read AFTER lowering rhs, for the exact same reason as lhsEndBlock -
    // rhs may itself be a nested short-circuit expression.
    llvm::BasicBlock* rhsEndBlock = builder.GetInsertBlock();
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = builder.CreatePHI(llvm::Type::getInt1Ty(context_), 2, isAnd ? "and.result" : "or.result");
    llvm::Constant* shortCircuitValue =
        isAnd ? llvm::ConstantInt::getFalse(context_) : llvm::ConstantInt::getTrue(context_);
    phi->addIncoming(shortCircuitValue, lhsEndBlock);
    phi->addIncoming(*rhs, rhsEndBlock);

    return phi;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerIdentifierExpr(const ast::IdentifierExpr& identifier,
                                                                    const SemanticModel& model,
                                                                    llvm::IRBuilder<>& builder) {
    // Identity comes only from resolution - never identifier source text.
    const std::optional<SymbolId> id = model.resolution(identifier);
    if (!id.has_value()) {
        return std::nullopt;
    }

    const Symbol& symbol = model.symbol(*id);
    if (symbol.kind != SymbolKind::Local && symbol.kind != SymbolKind::Parameter) {
        // Function/Builtin: no modeled value as a plain identifier - no
        // first-class Function Type exists (TypeChecker itself records
        // Type::unresolved() for these, so lowerExpr()'s own gate would
        // already have caught them; this check is the direct, documented
        // reason, not a redundant guess).
        return std::nullopt;
    }

    // Local and Parameter share one storage table - a Parameter bound in
    // defineFunction() is indistinguishable from an ordinary local here.
    llvm::AllocaInst* slot = findLocalSlot(*id);
    if (slot == nullptr) {
        return std::nullopt;
    }

    // The slot's own allocated type drives the load - never independently
    // re-lowered from model.typeOf(), so a load's type can never drift
    // from the alloca it reads.
    return builder.CreateLoad(slot->getAllocatedType(), slot);
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerAssignmentExpr(const ast::AssignmentExpr& assignment,
                                                                    const SemanticModel& model,
                                                                    llvm::IRBuilder<>& builder) {
    if (assignment.target().kind() == ast::ExprKind::Index) {
        // KAI LANGUAGE M7B: `xs[index] = value` - a real, checked
        // element write. See lowerIndexAssignmentExpr()'s own doc
        // comment for the full design.
        return lowerIndexAssignmentExpr(static_cast<const ast::IndexExpr&>(assignment.target()), assignment.value(),
                                         model, builder);
    }

    const ast::IdentifierExpr* targetIdentifier = unwrapAssignmentTargetIdentifier(assignment.target());
    if (targetIdentifier == nullptr) {
        return std::nullopt;
    }

    const std::optional<SymbolId> id = model.resolution(*targetIdentifier);
    if (!id.has_value()) {
        return std::nullopt;
    }

    // A successfully type-checked AssignmentExpr guarantees the target is
    // a mutable, storage-backed Local (TypeChecker rejects Parameter
    // targets via AssignmentToImmutableBinding - parameters can never be
    // mutable per GRAMMAR.md §10 - and rejects every other target shape
    // outright) - so "no slot found" is sufficient, on its own, to reject
    // every case this milestone does not support. No separate
    // mutability/target-shape check is duplicated here.
    llvm::AllocaInst* slot = findLocalSlot(*id);
    if (slot == nullptr) {
        return std::nullopt;
    }

    // KAI LANGUAGE M8B: whole-array reassignment (`a = b`, including
    // self-assignment `a = a`) is now real - CreateLoad/CreateStore below
    // already support an aggregate SSA value structurally with zero
    // further changes, exactly like every other type this path handles.
    // KAI LANGUAGE M8A resolved the LANGUAGE semantics (ordinary
    // value-copy, no aliasing - TYPE_SYSTEM.md §19); M7B's own guard here
    // deliberately deferred the backend implementation until this
    // milestone rather than let it "fall out" unreviewed. Indexed ELEMENT
    // assignment (`xs[i] = v`) is handled entirely separately above and
    // was already fully supported.
    const std::optional<llvm::Value*> rhs = lowerExpr(assignment.value(), model, builder);
    if (!rhs.has_value() || *rhs == nullptr) {
        return std::nullopt;
    }
    if ((*rhs)->getType() != slot->getAllocatedType()) {
        return std::nullopt;
    }

    builder.CreateStore(*rhs, slot);

    // KAI assignment is Unit-valued, not a C-style value-producing
    // expression - the stored value is never returned as the result of
    // this expression. std::optional<llvm::Value*>{nullptr} is the
    // deliberate "successful Unit expression" representation this
    // design reserves distinctly from std::nullopt (failure).
    return std::optional<llvm::Value*>(nullptr);
}

std::optional<LLVMCodeGenerator::ArrayElementAddress> LLVMCodeGenerator::lowerArrayBase(
    const ast::Expr& object, const SemanticModel& model, llvm::IRBuilder<>& builder) {
    if (object.kind() == ast::ExprKind::Paren) {
        return lowerArrayBase(static_cast<const ast::ParenExpr&>(object).inner(), model, builder);
    }

    if (object.kind() == ast::ExprKind::Index) {
        // KAI LANGUAGE M9: `matrix[i][j]`'s outer IndexExpr has
        // `matrix[i]` as its own object - recurse to get the ALREADY
        // bounds-checked address of that nested array element (fully
        // resolved, including its own trap-guarded check and GEP, before
        // this call returns), and reuse it directly as this level's own
        // array storage. This is what generalizes indexing from "a
        // direct local/parameter identifier" to "any array-typed lvalue
        // built from nested IndexExpr layers" - no source-level
        // reference is ever introduced; the returned pointer is always,
        // structurally, an address inside the root identifier's own
        // allocation.
        const std::optional<ArrayElementAddress> nested =
            lowerArrayElementAddress(static_cast<const ast::IndexExpr&>(object), model, builder);
        if (!nested.has_value() || !llvm::isa<llvm::ArrayType>(nested->elementType)) {
            // The `!isa<ArrayType>` branch is defensive only: checkIndexExpr()
            // already guarantees `object` is itself array-typed whenever its
            // OWN object indexed successfully, so this cannot happen for a
            // program that reached codegen at all.
            return std::nullopt;
        }
        return nested;
    }

    if (object.kind() != ast::ExprKind::Identifier) {
        // A call/member/... base - M9 does not introduce general lvalue
        // references, matching the M7B scope this generalizes.
        return std::nullopt;
    }

    // KAI LANGUAGE M8B: the root identifier may resolve to a
    // SymbolKind::Local OR a SymbolKind::Parameter array binding - array
    // function parameters need element READS to work the same way a
    // local's do, and both are bound into the same `locals_` table via
    // the same entry-alloca+store pattern (see declareFunction()'s
    // parameter-binding loop), so `findLocalSlot()` already finds either
    // uniformly. This does NOT reopen element WRITES through a
    // parameter: lowerIndexAssignmentExpr() also reaches this method (via
    // lowerArrayElementAddress()), but TypeChecker's
    // checkIndexAssignmentTarget() already rejects `xs[i] = v` (at any
    // nesting depth) for a non-Local root at the semantic layer - KAI
    // parameters remain immutable - so this broadened check can never
    // actually be reached for a write through a parameter in practice,
    // and is not itself a mutability decision.
    const auto& rootIdentifier = static_cast<const ast::IdentifierExpr&>(object);
    const std::optional<SymbolId> rootId = model.resolution(rootIdentifier);
    if (!rootId.has_value()) {
        return std::nullopt;
    }
    const SymbolKind rootKind = model.symbol(*rootId).kind;
    if (rootKind != SymbolKind::Local && rootKind != SymbolKind::Parameter) {
        return std::nullopt;
    }
    llvm::AllocaInst* arraySlot = findLocalSlot(*rootId);
    if (arraySlot == nullptr) {
        return std::nullopt;
    }
    auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(arraySlot->getAllocatedType());
    if (arrayType == nullptr) {
        return std::nullopt;
    }
    // The slot's own allocated type drives everything below - never
    // independently re-derived from model.arrayElementType()/
    // arrayLength(), so this can never structurally drift from the
    // actual storage it addresses (same discipline as
    // lowerIdentifierExpr()'s own "the slot's own allocated type drives
    // the load" rule).
    return ArrayElementAddress{arraySlot, arrayType};
}

llvm::Value* LLVMCodeGenerator::lowerCheckedIndexBounds(llvm::Value* indexValue, Type indexSemanticType,
                                                          llvm::Value* runtimeLength, llvm::IRBuilder<>& builder) {
    // M7B spec §6: normalize to an unsigned i64 for a single, width-safe
    // comparison against the length, regardless of the index's own
    // concrete width (including i64/u64 itself, where a plain
    // CreateSExt/CreateZExt would violate LLVM's own "strictly widening"
    // precondition) - CreateSExtOrTrunc/CreateZExtOrTrunc are exactly
    // LLVM's own answer to "the source may already be the target width."
    // A signed index is ADDITIONALLY checked for non-negativity at its
    // OWN width first: sign-extending a negative value to i64 would
    // itself already be numerically wrong to compare against an unsigned
    // length (e.g. i8 -1 sign-extends to the all-ones i64 pattern, which
    // is NOT "a huge positive number" once compared via an UNSIGNED
    // predicate - it IS the maximum u64 value, so it would incorrectly
    // compare "in bounds" against any nonzero length without this
    // separate, explicit sign check).
    llvm::Type* i64Type = llvm::Type::getInt64Ty(context_);
    llvm::Value* isNonNegative = llvm::ConstantInt::getTrue(context_);
    llvm::Value* indexAsI64;
    if (indexSemanticType.isSignedInteger()) {
        llvm::Value* zeroAtSourceWidth = llvm::ConstantInt::get(indexValue->getType(), 0);
        isNonNegative = builder.CreateICmpSGE(indexValue, zeroAtSourceWidth);
        indexAsI64 = builder.CreateSExtOrTrunc(indexValue, i64Type);
    } else {
        // An unsigned value is never negative - isNonNegative stays the
        // constant `true` above; no wrapping/reinterpretation involved.
        indexAsI64 = builder.CreateZExtOrTrunc(indexValue, i64Type);
    }
    llvm::Value* belowLength = builder.CreateICmpULT(indexAsI64, runtimeLength);
    llvm::Value* inBounds = builder.CreateAnd(isNonNegative, belowLength);

    // lowerExpr() (for the index, in the caller) may have moved `builder`
    // (a short-circuit index expression) - the CondBr below always
    // originates from wherever it ACTUALLY left `builder`, same
    // discipline as generateIfStmt()'s/generateWhileStmt()'s own
    // condition lowering.
    llvm::BasicBlock* checkEndBlock = builder.GetInsertBlock();
    llvm::Function* function = checkEndBlock->getParent();
    llvm::BasicBlock* inBoundsBlock = llvm::BasicBlock::Create(context_, "index.inbounds", function);
    llvm::BasicBlock* outOfBoundsBlock = llvm::BasicBlock::Create(context_, "index.outofbounds", function);
    builder.SetInsertPoint(checkEndBlock);
    builder.CreateCondBr(inBounds, inBoundsBlock, outOfBoundsBlock);

    // M7B spec §5: NOT KAI `panic` - a non-recoverable trap, no unwind,
    // no stable exit-code guarantee. No element address is ever computed
    // in this block, and this block never reaches a `ret`/back-edge/any
    // other continuation - `unreachable` is the actual, literal
    // terminator, not merely a naming convention.
    builder.SetInsertPoint(outOfBoundsBlock);
    llvm::Function* trapFn = llvm::Intrinsic::getOrInsertDeclaration(module_.get(), llvm::Intrinsic::trap);
    builder.CreateCall(trapFn);
    builder.CreateUnreachable();

    // Only reached once the bounds check has ALREADY succeeded - the
    // caller's own element-address GEP (and, from there, the load/store
    // through it) never executes on any path that didn't pass through
    // this branch.
    builder.SetInsertPoint(inBoundsBlock);
    return indexAsI64;
}

std::optional<LLVMCodeGenerator::ArrayElementAddress> LLVMCodeGenerator::lowerArrayElementAddress(
    const ast::IndexExpr& indexExpr, const SemanticModel& model, llvm::IRBuilder<>& builder) {
    // KAI LANGUAGE M9: lowerArrayBase() resolves `indexExpr.object()` -
    // either a direct Local/Parameter identifier (M7B/M8B), or, now, a
    // nested IndexExpr whose own address it fully resolves first (see
    // lowerArrayBase()'s own doc comment). Everything below is otherwise
    // unchanged from M7B: it addresses exactly ONE level of indexing into
    // whatever array `base` names.
    const std::optional<ArrayElementAddress> base = lowerArrayBase(indexExpr.object(), model, builder);
    if (!base.has_value()) {
        return std::nullopt;
    }
    auto* arrayType = llvm::cast<llvm::ArrayType>(base->elementType);
    llvm::Value* arraySlot = base->pointer;
    llvm::Type* elementType = arrayType->getElementType();
    const std::uint64_t length = arrayType->getNumElements();

    // The index expression is evaluated EXACTLY ONCE here, and the
    // resulting SSA value is reused for both the bounds comparison below
    // AND the GEP - never re-lowered (M7B spec §10/§11).
    const std::optional<llvm::Value*> indexValue = lowerExpr(indexExpr.index(), model, builder);
    if (!indexValue.has_value() || *indexValue == nullptr) {
        return std::nullopt;
    }
    const std::optional<Type> indexSemanticType = model.typeOf(indexExpr.index());
    if (!indexSemanticType.has_value() || !indexSemanticType->isInteger()) {
        // Defensive only - checkIndexExpr() already guarantees an
        // integer-domain index reaches here.
        return std::nullopt;
    }

    llvm::Value* lengthConstant = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), length);
    llvm::Value* indexAsI64 = lowerCheckedIndexBounds(*indexValue, *indexSemanticType, lengthConstant, builder);

    // `builder` is now positioned in the in-bounds successor block
    // (lowerCheckedIndexBounds()'s own postcondition) - the GEP (and, in
    // the caller, the load/store through it) never executes on any path
    // that didn't pass through that check.
    llvm::Value* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0);
    llvm::Value* elementAddress =
        builder.CreateGEP(arrayType, arraySlot, {zero32, indexAsI64}, "index.addr");

    return ArrayElementAddress{elementAddress, elementType};
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerIndexExpr(const ast::IndexExpr& index, const SemanticModel& model,
                                                               llvm::IRBuilder<>& builder) {
    // KAI LANGUAGE M10B: a Slice object dispatches to its own address
    // computation entirely - see lowerSliceIndexExpr()'s own doc comment.
    // `model.typeOf(index.object())` reads back a fact TypeChecker's
    // earlier pass already recorded - a side-effect-free lookup, never a
    // second type-check pass.
    const std::optional<Type> objectType = model.typeOf(index.object());
    if (objectType.has_value() && objectType->isSlice()) {
        return lowerSliceIndexExpr(index, model, builder);
    }

    const std::optional<ArrayElementAddress> address = lowerArrayElementAddress(index, model, builder);
    if (!address.has_value()) {
        return std::nullopt;
    }
    return builder.CreateLoad(address->elementType, address->pointer);
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerSliceIndexExpr(const ast::IndexExpr& index,
                                                                     const SemanticModel& model,
                                                                     llvm::IRBuilder<>& builder) {
    // The Slice object is lowered as an ORDINARY value (an identifier
    // load, a call result, ...) - never through array-storage machinery,
    // since a Slice is a small `{ptr,i64}` aggregate VALUE, not
    // alloca-backed array storage.
    const std::optional<llvm::Value*> sliceValue = lowerExpr(index.object(), model, builder);
    if (!sliceValue.has_value() || *sliceValue == nullptr) {
        return std::nullopt;
    }
    const std::optional<Type> objectType = model.typeOf(index.object());
    if (!objectType.has_value() || !objectType->isSlice()) {
        // Defensive only - lowerIndexExpr() already dispatched here
        // specifically because this was a Slice object.
        return std::nullopt;
    }
    llvm::Type* elementType = lowerType(model.sliceElementType(*objectType), model);
    if (elementType == nullptr) {
        return std::nullopt;
    }

    // The Slice's own `ptr`/`len` fields - `ptr` already addresses the
    // FIRST element directly (unlike an array's own alloca, which
    // addresses the whole aggregate), and `len` is genuinely RUNTIME
    // data, never a compile-time constant the way an array's own length
    // is (TYPE_SYSTEM.md's own "Slices" section).
    llvm::Value* dataPointer = builder.CreateExtractValue(*sliceValue, {0}, "slice.data");
    llvm::Value* runtimeLength = builder.CreateExtractValue(*sliceValue, {1}, "slice.len");

    // The index expression is evaluated EXACTLY ONCE here, and the
    // resulting SSA value is reused for both the bounds comparison
    // (inside lowerCheckedIndexBounds()) AND the GEP - never re-lowered,
    // mirroring lowerArrayElementAddress()'s own exact discipline (M10B
    // spec §19).
    const std::optional<llvm::Value*> indexValue = lowerExpr(index.index(), model, builder);
    if (!indexValue.has_value() || *indexValue == nullptr) {
        return std::nullopt;
    }
    const std::optional<Type> indexSemanticType = model.typeOf(index.index());
    if (!indexSemanticType.has_value() || !indexSemanticType->isInteger()) {
        // Defensive only - checkIndexExpr() already guarantees an
        // integer-domain index reaches here.
        return std::nullopt;
    }

    llvm::Value* indexAsI64 = lowerCheckedIndexBounds(*indexValue, *indexSemanticType, runtimeLength, builder);

    // `builder` is now positioned in the in-bounds successor block
    // (lowerCheckedIndexBounds()'s own postcondition, M10B spec §18: no
    // element address/load may occur before the check succeeds). A
    // single-index GEP - `dataPointer` already addresses the first
    // element, unlike an array's own two-index `{0, i}` GEP into its
    // whole-aggregate storage.
    llvm::Value* elementAddress = builder.CreateGEP(elementType, dataPointer, {indexAsI64}, "slice.index.addr");
    return builder.CreateLoad(elementType, elementAddress);
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerIndexAssignmentExpr(const ast::IndexExpr& indexTarget,
                                                                         const ast::Expr& value,
                                                                         const SemanticModel& model,
                                                                         llvm::IRBuilder<>& builder) {
    const std::optional<ArrayElementAddress> address = lowerArrayElementAddress(indexTarget, model, builder);
    if (!address.has_value()) {
        return std::nullopt;
    }

    // `value` is lowered EXACTLY ONCE, only now that the bounds check has
    // already succeeded (`builder` is positioned in the in-bounds block
    // by lowerArrayElementAddress() itself) - M7B spec §11.
    const std::optional<llvm::Value*> rhs = lowerExpr(value, model, builder);
    if (!rhs.has_value() || *rhs == nullptr) {
        return std::nullopt;
    }
    if ((*rhs)->getType() != address->elementType) {
        return std::nullopt;
    }

    builder.CreateStore(*rhs, address->pointer);

    // Unit-valued, matching lowerAssignmentExpr()'s own identifier-target
    // convention exactly.
    return std::optional<llvm::Value*>(nullptr);
}

// KAI LANGUAGE M8B: makes ArrayLiteralExpr a genuine value-producing
// expression - see this method's own doc comment in the header for the
// full rationale (temp entry-block alloca, reuse
// lowerArrayLiteralIntoStorage() rather than a second element-store
// implementation, one final load). `type` is the ArrayLiteralExpr's own
// already-resolved Type (from model.typeOf()), exactly like every other
// lowerExpr() case that takes a `type` parameter - never re-derived here.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerArrayLiteralExpr(const ast::ArrayLiteralExpr& array, Type type,
                                                                      const SemanticModel& model,
                                                                      llvm::IRBuilder<>& builder) {
    llvm::Type* llvmType = lowerType(type, model);
    auto* arrayType = llvm::dyn_cast_or_null<llvm::ArrayType>(llvmType);
    if (arrayType == nullptr) {
        // nullptr: an unsupported element type (e.g. Char) somewhere in
        // this literal's type. Not an ArrayType at all would mean `type`
        // disagrees with this being an ArrayLiteralExpr in the first
        // place - SemanticAnalyzer/TypeChecker never produce that, but
        // this is rejected defensively rather than assumed.
        return std::nullopt;
    }

    // createEntryBlockAlloca() targets the function's own entry block
    // directly, independent of `builder`'s current insertion point - safe
    // to call here, mid-expression-lowering, exactly like every other
    // local-storage allocation in this class.
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::AllocaInst* temp = createEntryBlockAlloca(*function, arrayType, "array.literal.tmp");
    if (!lowerArrayLiteralIntoStorage(array, temp, arrayType, model, builder)) {
        return std::nullopt;
    }

    // One load of the complete aggregate - the temporary itself is never
    // exposed or aliased beyond this single point; the resulting SSA
    // value is an ordinary independent copy, same as any other value this
    // class produces.
    return builder.CreateLoad(arrayType, temp, "array.literal.val");
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerCallExpr(const ast::CallExpr& call, Type type,
                                                              const SemanticModel& model, llvm::IRBuilder<>& builder) {
    // Structural-only unwrap, mirroring TypeChecker's own callee
    // classification shape - never a semantic decision.
    const ast::IdentifierExpr* calleeIdentifier = unwrapDirectCalleeIdentifier(call.callee());
    if (calleeIdentifier == nullptr) {
        // A non-direct callee (a Binary/Call/Member/... expression) -
        // method/first-class-function-value calls remain deferred.
        return std::nullopt;
    }

    const std::optional<SymbolId> calleeId = model.resolution(*calleeIdentifier);
    if (!calleeId.has_value()) {
        return std::nullopt;
    }

    const Symbol& calleeSymbol = model.symbol(*calleeId);
    if (calleeSymbol.kind == SymbolKind::Builtin) {
        // `type` is forwarded (KAI LANGUAGE M10B) - lowerSliceCall()
        // needs the call's own concrete Slice Type; see
        // lowerBuiltinCallExpr()'s own doc comment for the full picture.
        return lowerBuiltinCallExpr(call, calleeSymbol, type, model, builder);
    }
    if (calleeSymbol.kind != SymbolKind::Function) {
        // Local/Parameter: not callable in this milestone (no first-class
        // Function Type exists to call through).
        return std::nullopt;
    }

    // Found by SymbolId, populated in PASS 1 (declareFunction()) before
    // ANY body lowers - this, and only this, is what makes a forward
    // call (`answer` declared textually after `main`) or a recursive
    // call (a function calling itself, found in `functions_` before its
    // own body even starts lowering) work with no special-casing here.
    llvm::Function* function = findFunction(*calleeId);
    if (function == nullptr) {
        return std::nullopt;
    }

    assert(calleeSymbol.signature.has_value());
    const FunctionSignature& signature = *calleeSymbol.signature;

    // Frontend already validated arity - re-checked here only as a
    // structural LLVM-level defensive invariant, never re-running
    // semantic argument-compatibility checking.
    if (call.arguments().size() != signature.parameterTypes.size()) {
        return std::nullopt;
    }

    std::vector<llvm::Value*> argumentValues;
    argumentValues.reserve(call.arguments().size());
    for (std::size_t i = 0; i < call.arguments().size(); ++i) {
        const std::optional<llvm::Value*> argument = lowerExpr(*call.arguments()[i], model, builder);
        if (!argument.has_value() || *argument == nullptr) {
            return std::nullopt;
        }
        llvm::Type* expectedType = lowerType(signature.parameterTypes[i], model);
        if (expectedType == nullptr || (*argument)->getType() != expectedType) {
            return std::nullopt;
        }
        argumentValues.push_back(*argument);
    }

    llvm::CallInst* callInst = builder.CreateCall(function, argumentValues);

    if (function->getReturnType()->isVoidTy()) {
        // LLVM call to void legitimately has no value usable as a
        // first-class expression - Unit success, never a fabricated
        // value, so `do_work()` still codegens successfully as an
        // ExprStmt (generateExprStmt() already accepts a Unit success).
        return std::optional<llvm::Value*>(nullptr);
    }

    llvm::Type* expectedResultType = lowerType(type, model);
    if (expectedResultType == nullptr || expectedResultType != function->getReturnType()) {
        // Unreachable given a successfully checked frontend (CallExpr's
        // own recorded Type always matches FunctionSignature.returnType
        // on success) - a defensive structural invariant, not a
        // semantic re-check.
        return std::nullopt;
    }

    return callInst;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerBuiltinCallExpr(const ast::CallExpr& call,
                                                                     const Symbol& builtinSymbol, Type type,
                                                                     const SemanticModel& model,
                                                                     llvm::IRBuilder<>& builder) {
    if (builtinSymbol.name == "print") {
        return lowerPrintCall(call, model, builder);
    }
    if (builtinSymbol.name == "slice") {
        return lowerSliceCall(call, type, model, builder);
    }
    if (builtinSymbol.name == "len") {
        return lowerLenCall(call, model, builder);
    }
    // `panic`/`assert`/any other future prelude name: recognized (via
    // SymbolKind::Builtin) but not yet lowerable - explicit failure, M6
    // spec §17: never a silently-skipped/no-op builtin call.
    return std::nullopt;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerPrintCall(const ast::CallExpr& call, const SemanticModel& model,
                                                               llvm::IRBuilder<>& builder) {
    // print has no committed FunctionSignature to validate against
    // (STANDARD_LIBRARY.md §3) - this arity check IS this builtin's
    // entire argument-count validation, not a general type checker.
    if (call.arguments().size() != 1) {
        return std::nullopt;
    }

    const ast::Expr& argumentExpr = *call.arguments()[0];
    const std::optional<Type> argumentType = model.typeOf(argumentExpr);
    if (!argumentType.has_value() || argumentType->isError() || argumentType->isUnresolved()) {
        return std::nullopt;
    }

    const std::optional<llvm::Value*> argument = lowerExpr(argumentExpr, model, builder);
    if (!argument.has_value() || *argument == nullptr) {
        return std::nullopt;
    }

    llvm::Type* i64Type = llvm::Type::getInt64Ty(context_);
    llvm::Type* i32Type = llvm::Type::getInt32Ty(context_);
    llvm::Type* doubleType = llvm::Type::getDoubleTy(context_);
    llvm::Type* ptrType = llvm::PointerType::get(context_, 0);
    llvm::Type* voidType = llvm::Type::getVoidTy(context_);

    if (argumentType->isStr()) {
        // Minimal String Literal Support milestone: Str is the one
        // print-argument Type that is not a single scalar - extract the
        // { ptr, i64 } descriptor's two fields (see lowerType()'s own
        // comment) and call the length-explicit runtime entry point
        // (kai_runtime.h's kai_print_str()) directly, never
        // strlen/NUL-terminated printing, so an embedded \0 byte prints
        // correctly instead of truncating (M8 spec #6).
        llvm::Value* dataPointer = builder.CreateExtractValue(*argument, {0});
        llvm::Value* byteLength = builder.CreateExtractValue(*argument, {1});
        llvm::FunctionCallee printStrFn = module_->getOrInsertFunction(
            "kai_print_str", llvm::FunctionType::get(voidType, {ptrType, i64Type}, /*isVarArg=*/false));
        builder.CreateCall(printStrFn, {dataPointer, byteLength});
        return std::optional<llvm::Value*>(nullptr);
    }

    llvm::Value* runtimeArgument = nullptr;
    llvm::FunctionCallee runtimeFn;
    if (argumentType->isSignedInteger()) {
        runtimeArgument = (*argument)->getType() == i64Type ? *argument : builder.CreateSExt(*argument, i64Type);
        runtimeFn = module_->getOrInsertFunction("kai_print_i64",
                                                  llvm::FunctionType::get(voidType, {i64Type}, /*isVarArg=*/false));
    } else if (argumentType->isUnsignedInteger()) {
        runtimeArgument = (*argument)->getType() == i64Type ? *argument : builder.CreateZExt(*argument, i64Type);
        runtimeFn = module_->getOrInsertFunction("kai_print_u64",
                                                  llvm::FunctionType::get(voidType, {i64Type}, /*isVarArg=*/false));
    } else if (argumentType->isBool()) {
        runtimeArgument = builder.CreateZExt(*argument, i32Type);
        runtimeFn = module_->getOrInsertFunction("kai_print_bool",
                                                  llvm::FunctionType::get(voidType, {i32Type}, /*isVarArg=*/false));
    } else if (argumentType->isFloat()) {
        runtimeArgument =
            (*argument)->getType() == doubleType ? *argument : builder.CreateFPExt(*argument, doubleType);
        runtimeFn = module_->getOrInsertFunction("kai_print_f64",
                                                  llvm::FunctionType::get(voidType, {doubleType}, /*isVarArg=*/false));
    } else {
        // Char/any other unsupported print argument Type - explicit
        // failure, never a silently-skipped print (M6 spec §2: char
        // printing is not required for this milestone). Str is handled
        // above, before this chain.
        return std::nullopt;
    }

    builder.CreateCall(runtimeFn, {runtimeArgument});

    // print is an effectful, Unit-valued builtin (M6 spec §10) - never
    // surface the runtime function's own (void) C ABI return as a KAI
    // value. Same std::optional<llvm::Value*>{nullptr} convention as
    // lowerAssignmentExpr()/a Unit-returning user function call.
    return std::optional<llvm::Value*>(nullptr);
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerSliceCall(const ast::CallExpr& call, Type type,
                                                                const SemanticModel& model,
                                                                llvm::IRBuilder<>& builder) {
    // TypeChecker's checkSliceBuiltinCall() already guarantees exactly
    // one argument - re-checked defensively rather than trusted blindly,
    // same discipline as every other lowerX() method in this class.
    if (call.arguments().size() != 1) {
        return std::nullopt;
    }

    // KAI LANGUAGE M10B spec §3: the ONLY eligible source, a direct
    // (through transparent ParenExpr only) identifier resolving to a
    // SymbolKind::Local or SymbolKind::Parameter array binding - already
    // validated by TypeChecker, trusted structurally here exactly like
    // lowerArrayBase()'s own identifier-root case trusts its own
    // frontend precondition.
    const ast::IdentifierExpr* rootIdentifier = unwrapSliceSourceIdentifier(*call.arguments()[0]);
    if (rootIdentifier == nullptr) {
        return std::nullopt;
    }
    const std::optional<SymbolId> id = model.resolution(*rootIdentifier);
    if (!id.has_value()) {
        return std::nullopt;
    }
    const SymbolKind rootKind = model.symbol(*id).kind;
    if (rootKind != SymbolKind::Local && rootKind != SymbolKind::Parameter) {
        return std::nullopt;
    }
    llvm::AllocaInst* arraySlot = findLocalSlot(*id);
    if (arraySlot == nullptr) {
        return std::nullopt;
    }
    auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(arraySlot->getAllocatedType());
    if (arrayType == nullptr) {
        return std::nullopt;
    }

    llvm::Type* sliceType = lowerType(type, model);
    if (sliceType == nullptr) {
        return std::nullopt;
    }

    // KAI LANGUAGE M10B spec §25: the address of the array's OWN backing
    // storage directly - never a copy into a temporary, never a heap
    // allocation. `{0, 0}`: the first index selects the (only) array
    // object itself, the second selects its first element - the SAME GEP
    // shape array indexing's own `index.addr` uses with a constant 0,
    // just without any bounds check (a zero-length array's own "first
    // element" address is still well-defined POINTER ARITHMETIC in LLVM,
    // never dereferenced unless an actual in-bounds element access
    // follows - spec §26: "need not be dereferenceable").
    llvm::Type* i32Type = llvm::Type::getInt32Ty(context_);
    llvm::Value* zero32 = llvm::ConstantInt::get(i32Type, 0);
    llvm::Value* dataPointer = builder.CreateGEP(arrayType, arraySlot, {zero32, zero32}, "slice.ptr");

    // The array's own COMPILE-TIME length (TYPE_SYSTEM.md §18) - never
    // read from runtime memory.
    llvm::Value* length = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), arrayType->getNumElements());

    // Not llvm::ConstantStruct::get() (lowerLiteralExpr()'s own approach
    // for a string literal's {ptr,i64}): `dataPointer` here is a runtime
    // SSA address (an alloca-derived GEP), never a compile-time constant
    // the way a string literal's own global-backed pointer is - CreateInsertValue
    // into a PoisonValue base (immediately, fully overwritten by both
    // inserts before any use) is the ordinary IRBuilder idiom for
    // building a non-constant aggregate value.
    llvm::Value* result = llvm::PoisonValue::get(sliceType);
    result = builder.CreateInsertValue(result, dataPointer, {0});
    result = builder.CreateInsertValue(result, length, {1});
    return result;
}

std::optional<llvm::Value*> LLVMCodeGenerator::lowerLenCall(const ast::CallExpr& call, const SemanticModel& model,
                                                              llvm::IRBuilder<>& builder) {
    if (call.arguments().size() != 1) {
        return std::nullopt;
    }

    const ast::Expr& argument = *call.arguments()[0];
    const std::optional<Type> argumentType = model.typeOf(argument);
    if (!argumentType.has_value() || argumentType->isError() || argumentType->isUnresolved()) {
        return std::nullopt;
    }

    llvm::Type* i64Type = llvm::Type::getInt64Ty(context_);

    if (argumentType->isArray()) {
        // KAI LANGUAGE M10B spec §8/§23: a fixed array's length is
        // compile-time structural data (part of the TYPE itself, M7A) -
        // `len()` never inspects runtime memory for it, and never
        // constructs/inspects the array's own VALUE either (defensively
        // rejecting the one remaining way an EXECUTABLE array-of-Slice
        // aggregate could otherwise be materialized just to compute a
        // length that was already known from the type alone - spec
        // §6/§29). `argument` is still LOWERED exactly once, for its own
        // independent side effects (this codebase's "never silently skip
        // an expression's evaluation" discipline), but the resulting
        // VALUE is deliberately discarded.
        if (typeContainsSlice(*argumentType, model)) {
            return std::nullopt;
        }
        const std::optional<llvm::Value*> discarded = lowerExpr(argument, model, builder);
        if (!discarded.has_value()) {
            return std::nullopt;
        }
        return llvm::ConstantInt::get(i64Type, model.arrayLength(*argumentType));
    }

    const std::optional<llvm::Value*> argumentValue = lowerExpr(argument, model, builder);
    if (!argumentValue.has_value() || *argumentValue == nullptr) {
        return std::nullopt;
    }

    if (argumentType->isSlice() || argumentType->isStr()) {
        // Both Slice's `{ptr, i64 elementCount}` and Str's `{ptr, i64
        // byteLength}` already store their own runtime length at field
        // index 1 - reuse the EXACT same extraction lowerPrintCall()
        // already established for Str, never a second implementation.
        return builder.CreateExtractValue(*argumentValue, {1});
    }

    // Unreachable given a successfully checked frontend
    // (checkLenBuiltinCall() only ever accepts Array/Slice/Str) - a
    // defensive structural invariant, not a semantic re-check.
    return std::nullopt;
}

} // namespace kai::codegen
