#include "kai/codegen/LLVMCodeGenerator.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <charconv>
#include <optional>
#include <string>

namespace kai::codegen {

using semantic::SemanticModel;
using semantic::Type;
using semantic::TypeKind;

LLVMCodeGenerator::LLVMCodeGenerator(const SourceManager& sources) : sources_(sources) {}

bool LLVMCodeGenerator::generate(const ast::SourceFile& file, const SemanticModel& model) {
    module_ = std::make_unique<llvm::Module>(std::string(sources_.fileName(file.file())), context_);

    bool ok = true;
    for (const auto& decl : file.declarations()) {
        if (!generateTopLevelDecl(*decl, model)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        std::string verifierErrors;
        llvm::raw_string_ostream errorStream(verifierErrors);
        ok = !llvm::verifyModule(*module_, &errorStream);
    }

    if (!ok) {
        module_.reset();
        return false;
    }
    return true;
}

const llvm::Module& LLVMCodeGenerator::module() const {
    assert(module_ != nullptr);
    return *module_;
}

// No `default:` case: DeclKind is fully implemented today, mirroring
// SemanticAnalyzer.cpp's/TypeChecker.cpp's/ControlFlowAnalyzer.cpp's own
// exhaustive switch over it.
bool LLVMCodeGenerator::generateTopLevelDecl(const ast::Decl& decl, const SemanticModel& model) {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            return generateFunction(static_cast<const ast::FunctionDecl&>(decl), model);
    }
    return false;
}

bool LLVMCodeGenerator::generateFunction(const ast::FunctionDecl& fn, const SemanticModel& model) {
    // Declaration mapping, not a name lookup - mirrors TypeChecker's and
    // ControlFlowAnalyzer's own established pattern.
    const std::optional<semantic::SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    const semantic::Symbol& symbol = model.symbol(*fnId);
    assert(symbol.signature.has_value());
    const semantic::FunctionSignature& signature = *symbol.signature;

    // M1/M2: zero-parameter functions only - parameter lowering is out of
    // scope for these milestones, not merely unimplemented by oversight.
    if (!signature.parameterTypes.empty()) {
        return false;
    }

    llvm::Type* returnType = lowerType(signature.returnType);
    if (returnType == nullptr) {
        return false;
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, /*isVarArg=*/false);
    const std::string name(sources_.text(fn.name().span));
    llvm::Function* function = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, *module_);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", function);

    // M1/M2: a body of exactly one ReturnStmt. Anything else (zero
    // statements, more than one statement, or a non-Return statement)
    // fails explicitly rather than emitting an incomplete function body
    // or a second terminator into `entry`. Locals/control-flow statements
    // remain M3+.
    const std::vector<ast::StmtPtr>& statements = fn.body().statements();
    if (statements.size() != 1 || statements[0]->kind() != ast::StmtKind::Return) {
        return false;
    }

    return generateReturnStmt(static_cast<const ast::ReturnStmt&>(*statements[0]), *entry, model);
}

bool LLVMCodeGenerator::generateReturnStmt(const ast::ReturnStmt& stmt, llvm::BasicBlock& block,
                                            const SemanticModel& model) {
    // M1/M2: bare `return` (Unit) is out of scope - only a valued,
    // expression-producing return is supported.
    const ast::Expr* value = stmt.value();
    if (value == nullptr) {
        return false;
    }

    llvm::IRBuilder<> builder(&block);

    // The one expression dispatcher handles every M2-supported shape
    // (literals, parens, unary, binary) uniformly - no return-specific
    // operator logic is duplicated here.
    const std::optional<llvm::Value*> result = lowerExpr(*value, model, builder);
    if (!result.has_value()) {
        return false;
    }

    if ((*result)->getType() != block.getParent()->getReturnType()) {
        return false;
    }

    builder.CreateRet(*result);
    return true;
}

// No `default:` case: ExprKind is fully implemented today, mirroring the
// rest of this codebase's exhaustive-switch convention.
std::optional<llvm::Value*> LLVMCodeGenerator::lowerExpr(const ast::Expr& expr, const SemanticModel& model,
                                                          llvm::IRBuilder<>& builder) {
    const std::optional<Type> type = model.typeOf(expr);
    if (!type.has_value() || type->isError() || type->isUnresolved()) {
        return std::nullopt;
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

        // Explicitly deferred to later milestones (M3+): identifier loads
        // and calls need locals/functions-as-values; assignment needs
        // mutable storage; array/index/member need compound types; Unit
        // is a legitimate value this milestone simply does not produce
        // yet; error propagation needs Result.
        case ast::ExprKind::Identifier:
        case ast::ExprKind::Call:
        case ast::ExprKind::Assignment:
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
            // valid, in-range literal of `type` (spec #18 in this
            // milestone's design) - codegen never re-derives range
            // validity or contextual typing here, it only re-reads the
            // digits TypeChecker already validated.
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
            // Explicitly deferred - not part of M2's primitive scope.
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
    if (!operand.has_value()) {
        return std::nullopt;
    }

    // Semantic ground truth from TypeChecker - never re-inferred here.
    // TypeChecker's Negate rule is "type in = type out" and Not always
    // requires/produces Bool (see checkUnaryExpr() in TypeChecker.cpp),
    // so the operand's own recorded Type is exactly what selects the
    // correct LLVM instruction for either operator.
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

    const std::optional<llvm::Value*> left = lowerExpr(binary.left(), model, builder);
    if (!left.has_value()) {
        return std::nullopt;
    }
    const std::optional<llvm::Value*> right = lowerExpr(binary.right(), model, builder);
    if (!right.has_value()) {
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
            // lowerLiteralExpr(), and identifiers/calls are deferred
            // entirely), so a Char operandType can never actually reach
            // here with both operands already lowered - the isInteger()/
            // isBool()/isFloat() checks below are exhaustive in practice
            // for this milestone, and Char falls through to the explicit
            // failure like any other unsupported case.
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
            // Eager (non-short-circuiting) `and` on i1 operands - see
            // this class's own header comment for why this is a safe,
            // explicitly provisional choice for M2's exact expression
            // grammar (both operands are already fully, unconditionally
            // lowered above regardless of operator, and nothing M2 can
            // express has an observable side effect), and why it must be
            // revisited before a later milestone puts a call expression
            // in logical operand position.
            return builder.CreateAnd(*left, *right);

        case ast::BinaryOperator::Or:
            return builder.CreateOr(*left, *right);

        case ast::BinaryOperator::Range:
            return std::nullopt; // handled above; unreachable
    }
    return std::nullopt;
}

// No `default:` case: TypeKind is fully implemented today, mirroring
// TypeChecker.cpp's own exhaustive switch over it (see integerRangeFor()).
llvm::Type* LLVMCodeGenerator::lowerType(Type type) {
    switch (type.kind()) {
        case TypeKind::Unit:
            return llvm::Type::getVoidTy(context_);
        case TypeKind::I8:
        case TypeKind::U8:
            return llvm::Type::getInt8Ty(context_);
        case TypeKind::I16:
        case TypeKind::U16:
            return llvm::Type::getInt16Ty(context_);
        case TypeKind::I32:
        case TypeKind::U32:
            return llvm::Type::getInt32Ty(context_);
        case TypeKind::I64:
        case TypeKind::U64:
            return llvm::Type::getInt64Ty(context_);
        case TypeKind::F32:
            return llvm::Type::getFloatTy(context_);
        case TypeKind::F64:
            return llvm::Type::getDoubleTy(context_);
        case TypeKind::Bool:
            return llvm::Type::getInt1Ty(context_);
        case TypeKind::Unresolved:
        case TypeKind::Error:
        case TypeKind::Char:
            return nullptr;
    }
    return nullptr;
}

} // namespace kai::codegen
