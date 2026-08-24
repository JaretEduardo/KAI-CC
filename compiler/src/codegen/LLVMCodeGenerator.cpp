#include "kai/codegen/LLVMCodeGenerator.hpp"

#include "kai/ast/Expr.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
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

    // M1: zero-parameter functions only - parameter lowering is out of
    // scope for this milestone, not merely unimplemented by oversight.
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

    // M1: a body of exactly one ReturnStmt. Anything else (zero
    // statements, more than one statement, or a non-Return statement)
    // fails explicitly rather than emitting an incomplete function body
    // or a second terminator into `entry`.
    const std::vector<ast::StmtPtr>& statements = fn.body().statements();
    if (statements.size() != 1 || statements[0]->kind() != ast::StmtKind::Return) {
        return false;
    }

    return generateReturnStmt(static_cast<const ast::ReturnStmt&>(*statements[0]), *entry, model);
}

bool LLVMCodeGenerator::generateReturnStmt(const ast::ReturnStmt& stmt, llvm::BasicBlock& block,
                                            const SemanticModel& model) {
    // M1: bare `return` (Unit) is out of scope - only a valued,
    // integer-literal return is supported.
    const ast::Expr* value = stmt.value();
    if (value == nullptr || value->kind() != ast::ExprKind::Literal) {
        return false;
    }

    const auto& literal = static_cast<const ast::LiteralExpr&>(*value);
    if (literal.literalKind() != ast::LiteralKind::Integer) {
        return false;
    }

    // Semantic ground truth from TypeChecker - never re-inferred here.
    const std::optional<Type> literalType = model.typeOf(*value);
    if (!literalType.has_value() || !literalType->isInteger()) {
        return false;
    }

    llvm::Type* llvmType = lowerType(*literalType);
    if (llvmType == nullptr || llvmType != block.getParent()->getReturnType()) {
        return false;
    }

    // LiteralExpr never decodes its own value (see its class comment) -
    // recover the literal's source text and decode it exactly the way
    // TypeChecker.cpp's decodeIntegerMagnitude() already does, rather
    // than inventing a second integer-literal-decoding algorithm here.
    const std::string_view text = sources_.text(literal.span());
    std::uint64_t magnitude = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), magnitude);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }

    llvm::IRBuilder<> builder(&block);
    builder.CreateRet(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(llvmType), magnitude,
                                              literalType->isSignedInteger()));
    return true;
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
