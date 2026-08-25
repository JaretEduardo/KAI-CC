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
#include <llvm/IR/Function.h>
#include <llvm/Support/Casting.h>

#include <cassert>
#include <charconv>
#include <cstddef>
#include <optional>

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
            return lowerLiteralExpr(static_cast<const ast::LiteralExpr&>(expr), *type, builder);
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

        // Explicitly deferred to later milestones (M5+): array/index/
        // member need compound types; Unit is a legitimate value this
        // milestone simply does not produce as an expression's own
        // literal form yet; error propagation needs Result.
        case ast::ExprKind::ArrayLiteral:
        case ast::ExprKind::Index:
        case ast::ExprKind::Member:
        case ast::ExprKind::Unit:
        case ast::ExprKind::ErrorPropagation:
            return std::nullopt;
    }
    return std::nullopt;
}

// No `default:` case: ast::LiteralKind is fully implemented today.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerLiteralExpr(const ast::LiteralExpr& literal, Type type,
                                                                 llvm::IRBuilder<>&) {
    llvm::Type* llvmType = lowerType(type);
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

        case ast::LiteralKind::String:
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
        // `type` (the CALL's own Type) is deliberately unused on this
        // path - see lowerBuiltinCallExpr()'s own doc comment.
        return lowerBuiltinCallExpr(call, calleeSymbol, model, builder);
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
        llvm::Type* expectedType = lowerType(signature.parameterTypes[i]);
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

    llvm::Type* expectedResultType = lowerType(type);
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
                                                                     const Symbol& builtinSymbol,
                                                                     const SemanticModel& model,
                                                                     llvm::IRBuilder<>& builder) {
    if (builtinSymbol.name == "print") {
        return lowerPrintCall(call, model, builder);
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
    llvm::Type* voidType = llvm::Type::getVoidTy(context_);

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
        // Char/String/any other unsupported print argument Type -
        // explicit failure, never a silently-skipped print (M6 spec §2:
        // char/string printing is not required for this milestone).
        return std::nullopt;
    }

    builder.CreateCall(runtimeFn, {runtimeArgument});

    // print is an effectful, Unit-valued builtin (M6 spec §10) - never
    // surface the runtime function's own (void) C ABI return as a KAI
    // value. Same std::optional<llvm::Value*>{nullptr} convention as
    // lowerAssignmentExpr()/a Unit-returning user function call.
    return std::optional<llvm::Value*>(nullptr);
}

} // namespace kai::codegen
