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
using semantic::Symbol;
using semantic::SymbolId;
using semantic::SymbolKind;
using semantic::Type;
using semantic::TypeKind;

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

} // namespace

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
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    const Symbol& symbol = model.symbol(*fnId);
    assert(symbol.signature.has_value());
    const semantic::FunctionSignature& signature = *symbol.signature;

    // M1-M3: zero-parameter functions only - parameter lowering is out of
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

    // Fresh per function - a slot from a previous function must never be
    // visible to this one.
    locals_.clear();

    llvm::IRBuilder<> builder(entry);
    return generateBlock(fn.body(), *function, builder, model);
}

bool LLVMCodeGenerator::generateBlock(const ast::BlockStmt& block, llvm::Function& function,
                                       llvm::IRBuilder<>& builder, const SemanticModel& model) {
    for (const auto& stmt : block.statements()) {
        if (builder.GetInsertBlock()->getTerminator() != nullptr) {
            // A prior ReturnStmt already terminated this block. The
            // frontend still fully checked every statement that follows
            // (M5's "no unreachable-code analysis" stance) - M3 simply
            // stops LOWERING them here rather than append instructions
            // after an LLVM terminator, which would be invalid IR.
            break;
        }
        if (!generateStatement(*stmt, function, builder, model)) {
            return false;
        }
    }
    return true;
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// TypeChecker.cpp's/ControlFlowAnalyzer.cpp's own exhaustive switch over
// it. If/While/For are explicit failures - control-flow lowering does not
// exist yet.
bool LLVMCodeGenerator::generateStatement(const ast::Stmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
                                           const SemanticModel& model) {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            // Recurses into the SAME BasicBlock/builder (M3 spec §15) -
            // no new block is created, since M3 has no control flow to
            // justify one. Note: KAI 0.1's current grammar has no
            // standalone `{ ... }` statement production (parseStatement()
            // only reaches parseBlock() via fn/if/while/for) - this case
            // exists for StmtKind switch-exhaustiveness and forward
            // compatibility, not because it is reachable from real source
            // text today.
            return generateBlock(static_cast<const ast::BlockStmt&>(stmt), function, builder, model);

        case ast::StmtKind::VarDecl:
            return generateVarDeclStmt(static_cast<const ast::VarDeclStmt&>(stmt), function, builder, model);

        case ast::StmtKind::Expr:
            return generateExprStmt(static_cast<const ast::ExprStmt&>(stmt), builder, model);

        case ast::StmtKind::Return:
            return generateReturnStmt(static_cast<const ast::ReturnStmt&>(stmt), builder, model);

        case ast::StmtKind::If:
        case ast::StmtKind::While:
        case ast::StmtKind::For:
            // Control flow remains deferred - never silently skipped.
            return false;
    }
    return false;
}

bool LLVMCodeGenerator::generateVarDeclStmt(const ast::VarDeclStmt& varDecl, llvm::Function& function,
                                             llvm::IRBuilder<>& builder, const SemanticModel& model) {
    // Declaration mapping, not a name lookup.
    const std::optional<SymbolId> id = model.declarationSymbol(varDecl.name());
    assert(id.has_value());

    // The Symbol's Type is TypeChecker's own already-resolved truth - for
    // an inferred `let x = 40` this is exactly the type
    // checkVarDecl()/setSymbolType() already assigned; for an annotated
    // `let x: i64 = 40` it is the resolved annotation. Both are handled
    // by this one code path - the initializer's own AST shape is never
    // inspected to decide the local's type.
    const Symbol& symbol = model.symbol(*id);
    llvm::Type* llvmType = lowerType(symbol.type);
    if (llvmType == nullptr || llvmType->isVoidTy()) {
        // nullptr: Error/Unresolved/Char, same as everywhere else.
        // isVoidTy(): a Unit-typed local - LLVM has no storable void
        // value, so this is an explicit, documented M3 policy failure,
        // not a fallthrough of the Error/Unresolved case (lowerType()
        // legitimately returns void for Unit, since Unit IS a valid
        // function return type - only a LOCAL's storage type rejects it).
        return false;
    }

    const std::optional<llvm::Value*> initializer = lowerExpr(varDecl.initializer(), model, builder);
    if (!initializer.has_value() || *initializer == nullptr) {
        // nullopt: lowering failure. nullptr: a Unit-valued initializer
        // (e.g. `let y = (x = 1)`) - already excluded by the isVoidTy()
        // check above in practice (such a `y` is itself Unit-typed), but
        // checked again here defensively rather than trusted implicitly.
        return false;
    }
    if ((*initializer)->getType() != llvmType) {
        return false;
    }

    llvm::AllocaInst* slot = createEntryBlockAlloca(function, llvmType, sources_.text(varDecl.name().span));
    builder.CreateStore(*initializer, slot);
    locals_.emplace_back(*id, slot);
    return true;
}

bool LLVMCodeGenerator::generateExprStmt(const ast::ExprStmt& stmt, llvm::IRBuilder<>& builder,
                                          const SemanticModel& model) {
    // The resulting value (if any) is discarded either way - only
    // lowering failure is statement failure. Covers both a value-
    // producing expression used for effect and a Unit-valued
    // AssignmentExpr (`y = y + 1`) used for its store side effect.
    return lowerExpr(stmt.expr(), model, builder).has_value();
}

bool LLVMCodeGenerator::generateReturnStmt(const ast::ReturnStmt& stmt, llvm::IRBuilder<>& builder,
                                            const SemanticModel& model) {
    // Bare `return` (Unit) remains out of scope - only a valued,
    // expression-producing return is supported.
    const ast::Expr* value = stmt.value();
    if (value == nullptr) {
        return false;
    }

    // The one expression dispatcher handles every supported shape
    // (literals, parens, unary, binary, identifiers, assignment)
    // uniformly - no return-specific operator/local logic is duplicated
    // here.
    const std::optional<llvm::Value*> result = lowerExpr(*value, model, builder);
    if (!result.has_value() || *result == nullptr) {
        // nullopt: lowering failure. nullptr: a Unit-valued return
        // expression - not supported (a non-Unit function could never
        // type-check one anyway; a Unit-returning function's bare
        // `return` already took the value-is-nullptr branch above).
        return false;
    }

    if ((*result)->getType() != builder.GetInsertBlock()->getParent()->getReturnType()) {
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
        case ast::ExprKind::Identifier:
            return lowerIdentifierExpr(static_cast<const ast::IdentifierExpr&>(expr), model, builder);
        case ast::ExprKind::Assignment:
            return lowerAssignmentExpr(static_cast<const ast::AssignmentExpr&>(expr), model, builder);

        // Explicitly deferred to later milestones (M4+): calls need
        // functions-as-values; array/index/member need compound types;
        // Unit is a legitimate value this milestone simply does not
        // produce as an expression's own literal form yet; error
        // propagation needs Result.
        case ast::ExprKind::Call:
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
            // lowerLiteralExpr(), and identifiers can only be Local/
            // Parameter, never a Char-typed literal source), so a Char
            // operandType can never actually reach here with both
            // operands already lowered - the isInteger()/isBool()/
            // isFloat() checks below are exhaustive in practice, and Char
            // falls through to the explicit failure like any other
            // unsupported case.
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
            // this class's own header comment for exactly why this
            // remains acceptable only through M3, and why it must be
            // resolved before call lowering merges.
            return builder.CreateAnd(*left, *right);

        case ast::BinaryOperator::Or:
            return builder.CreateOr(*left, *right);

        case ast::BinaryOperator::Range:
            return std::nullopt; // handled above; unreachable
    }
    return std::nullopt;
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
    if (symbol.kind != SymbolKind::Local) {
        // Parameter: no storage slot exists yet in M3 (parameters remain
        // unsupported until M4) - falls through to the same "no slot"
        // failure a missing Local would. Function/Builtin: no modeled
        // value as a plain identifier (TypeChecker itself records
        // Type::unresolved() for these, so lowerExpr()'s own gate would
        // already have caught them - this check is the direct,
        // documented reason, not a redundant guess).
        return std::nullopt;
    }

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
    // every case M3 does not support. No separate mutability/target-shape
    // check is duplicated here.
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
    // milestone's design reserves distinctly from std::nullopt (failure).
    return std::optional<llvm::Value*>(nullptr);
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

llvm::AllocaInst* LLVMCodeGenerator::createEntryBlockAlloca(llvm::Function& function, llvm::Type* type,
                                                             llvm::StringRef name) {
    llvm::BasicBlock& entry = function.getEntryBlock();
    llvm::IRBuilder<> entryBuilder(&entry, entry.begin());
    return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::AllocaInst* LLVMCodeGenerator::findLocalSlot(SymbolId id) const {
    for (const auto& [slotId, slot] : locals_) {
        if (slotId == id) {
            return slot;
        }
    }
    return nullptr;
}

} // namespace kai::codegen
