// Module/function orchestration and statement/block lowering. Expression
// lowering (lowerExpr() and everything it dispatches to - literals,
// identifiers, unary/binary/logical, assignment, calls) lives in the
// sibling LLVMExpressionLowering.cpp - a purely mechanical split (M4 spec
// §25) once this file passed ~700 lines: no public API change, every
// method here is still declared once, in LLVMCodeGenerator.hpp, and nothing
// was made public merely to enable the split.

#include "kai/codegen/LLVMCodeGenerator.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <cstddef>
#include <optional>
#include <string>

namespace kai::codegen {

using semantic::FunctionSignature;
using semantic::SemanticModel;
using semantic::Symbol;
using semantic::SymbolId;
using semantic::Type;
using semantic::TypeKind;

LLVMCodeGenerator::LLVMCodeGenerator(const SourceManager& sources) : sources_(sources) {}

bool LLVMCodeGenerator::generate(const ast::SourceFile& file, const SemanticModel& model) {
    module_ = std::make_unique<llvm::Module>(std::string(sources_.fileName(file.file())), context_);
    functions_.clear();

    // PASS 1: declare every function's signature before lowering ANY
    // body - this is what makes forward calls, recursion, and mutual
    // recursion work with no lazy "create callee on first use" scheme.
    bool ok = true;
    for (const auto& decl : file.declarations()) {
        if (!declareTopLevelDecl(*decl, model)) {
            ok = false;
            break;
        }
    }

    // PASS 2: lower every function body, now able to CreateCall any
    // function from PASS 1 regardless of declaration order.
    if (ok) {
        for (const auto& decl : file.declarations()) {
            if (!defineTopLevelDecl(*decl, model)) {
                ok = false;
                break;
            }
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
bool LLVMCodeGenerator::declareTopLevelDecl(const ast::Decl& decl, const SemanticModel& model) {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            return declareFunction(static_cast<const ast::FunctionDecl&>(decl), model);
    }
    return false;
}

bool LLVMCodeGenerator::defineTopLevelDecl(const ast::Decl& decl, const SemanticModel& model) {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            return defineFunction(static_cast<const ast::FunctionDecl&>(decl), model);
    }
    return false;
}

bool LLVMCodeGenerator::declareFunction(const ast::FunctionDecl& fn, const SemanticModel& model) {
    // Declaration mapping, not a name lookup - mirrors TypeChecker's and
    // ControlFlowAnalyzer's own established pattern.
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    const Symbol& symbol = model.symbol(*fnId);
    assert(symbol.signature.has_value());
    const FunctionSignature& signature = *symbol.signature;

    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(signature.parameterTypes.size());
    for (const Type paramType : signature.parameterTypes) {
        llvm::Type* llvmParamType = lowerType(paramType);
        if (llvmParamType == nullptr) {
            return false;
        }
        paramTypes.push_back(llvmParamType);
    }

    llvm::Type* returnType = lowerType(signature.returnType);
    if (returnType == nullptr) {
        return false;
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, paramTypes, /*isVarArg=*/false);
    const std::string name(sources_.text(fn.name().span));
    llvm::Function* function = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, *module_);

    // Readable LLVM argument names from the KAI source spelling - never
    // used for lookup, purely for IR legibility (see defineFunction()'s
    // parameter binding, which uses SymbolId, not these names).
    std::size_t index = 0;
    for (llvm::Argument& arg : function->args()) {
        arg.setName(sources_.text(fn.params()[index].name.span));
        ++index;
    }

    functions_.emplace_back(*fnId, function);
    return true;
}

bool LLVMCodeGenerator::defineFunction(const ast::FunctionDecl& fn, const SemanticModel& model) {
    const std::optional<SymbolId> fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    // PASS 1 (declareFunction) already ran for every FunctionDecl before
    // PASS 2 started (generate() aborts entirely if PASS 1 ever fails) -
    // so this is always found, never a fallback lookup.
    llvm::Function* function = findFunction(*fnId);
    assert(function != nullptr);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", function);

    // Fresh per function - a slot from a previous function must never be
    // visible to this one.
    locals_.clear();

    llvm::IRBuilder<> builder(entry);

    // Bind every parameter to entry-block storage exactly like an
    // ordinary local declaration does (M4 spec §4) - the SAME `locals_`
    // table, so lowerIdentifierExpr() needs no separate parameter-load
    // path. `arg.getType()` (not a fresh lowerType() call) is what
    // createEntryBlockAlloca() allocates, guaranteeing the slot's type
    // always matches the incoming argument exactly.
    const std::vector<ast::Param>& params = fn.params();
    std::size_t paramIndex = 0;
    for (llvm::Argument& arg : function->args()) {
        const std::optional<SymbolId> paramId = model.declarationSymbol(params[paramIndex].name);
        assert(paramId.has_value());
        llvm::AllocaInst* slot = createEntryBlockAlloca(*function, arg.getType(), arg.getName());
        builder.CreateStore(&arg, slot);
        locals_.emplace_back(*paramId, slot);
        ++paramIndex;
    }

    if (!generateBlock(fn.body(), *function, builder, model)) {
        return false;
    }

    // Function fallthrough policy (M4 spec §22/§23): `builder`'s current
    // block is read fresh here - never assumed to still be `entry` - a
    // short-circuit expression in the last statement may have left it
    // somewhere else entirely.
    llvm::BasicBlock* currentBlock = builder.GetInsertBlock();
    if (currentBlock->getTerminator() == nullptr) {
        if (function->getReturnType()->isVoidTy()) {
            // Not inventing KAI semantics: a Unit function is already
            // allowed to fall through by frontend semantics (M5 spec) -
            // LLVM merely requires every defined block to terminate.
            builder.CreateRetVoid();
        } else {
            // Unreachable after a successful ControlFlowAnalyzer for a
            // concrete non-Unit return type - never fabricate a return
            // value here.
            return false;
        }
    }

    return true;
}

bool LLVMCodeGenerator::generateBlock(const ast::BlockStmt& block, llvm::Function& function,
                                       llvm::IRBuilder<>& builder, const SemanticModel& model) {
    for (const auto& stmt : block.statements()) {
        if (builder.GetInsertBlock()->getTerminator() != nullptr) {
            // A prior ReturnStmt already terminated this block. The
            // frontend still fully checked every statement that follows
            // (M5's "no unreachable-code analysis" stance) - this pass
            // simply stops LOWERING them here rather than append
            // instructions after an LLVM terminator, which would be
            // invalid IR.
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
// it. If/While/For are explicit failures - statement-level control flow
// does not exist yet (M5).
bool LLVMCodeGenerator::generateStatement(const ast::Stmt& stmt, llvm::Function& function, llvm::IRBuilder<>& builder,
                                           const SemanticModel& model) {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            // Recurses into the SAME BasicBlock/builder - no new block is
            // created, since statement-level control flow does not exist
            // yet. Note: KAI 0.1's current grammar has no standalone
            // `{ ... }` statement production (parseStatement() only
            // reaches parseBlock() via fn/if/while/for) - this case
            // exists for StmtKind switch-exhaustiveness and forward
            // compatibility, not because it is reachable from real
            // source text today.
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
            // Statement-level control flow remains deferred to M5 - never
            // silently skipped.
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
        // value, so this is an explicit, documented policy failure, not
        // a fallthrough of the Error/Unresolved case (lowerType()
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

    // lowerExpr() may have moved `builder` to a different block (a
    // short-circuit initializer) - createEntryBlockAlloca() always
    // targets the literal function entry block regardless, but the store
    // itself correctly goes through `builder` at its now-current position.
    llvm::AllocaInst* slot = createEntryBlockAlloca(function, llvmType, sources_.text(varDecl.name().span));
    builder.CreateStore(*initializer, slot);
    locals_.emplace_back(*id, slot);
    return true;
}

bool LLVMCodeGenerator::generateExprStmt(const ast::ExprStmt& stmt, llvm::IRBuilder<>& builder,
                                          const SemanticModel& model) {
    // The resulting value (if any) is discarded either way - only
    // lowering failure is statement failure. Covers a value-producing
    // expression used for effect, a Unit-valued AssignmentExpr
    // (`y = y + 1`), and a Unit-returning call (`do_work()`) used for its
    // side effect.
    return lowerExpr(stmt.expr(), model, builder).has_value();
}

bool LLVMCodeGenerator::generateReturnStmt(const ast::ReturnStmt& stmt, llvm::IRBuilder<>& builder,
                                            const SemanticModel& model) {
    const ast::Expr* value = stmt.value();
    if (value == nullptr) {
        // A bare `return` is only ever valid (per TypeChecker) when the
        // enclosing function's declared return type is Unit - defensively
        // re-checked here rather than trusted blindly.
        if (!builder.GetInsertBlock()->getParent()->getReturnType()->isVoidTy()) {
            return false;
        }
        builder.CreateRetVoid();
        return true;
    }

    // The one expression dispatcher handles every supported shape
    // uniformly - no return-specific operator/local/call logic is
    // duplicated here.
    const std::optional<llvm::Value*> result = lowerExpr(*value, model, builder);
    if (!result.has_value() || *result == nullptr) {
        // nullopt: lowering failure. nullptr: a Unit-valued return
        // expression (e.g. `return do_work()` where do_work() -> ()) -
        // not supported: a non-Unit function could never type-check one
        // anyway, and a Unit-returning function's bare `return` already
        // took the value-is-nullptr branch above (a Unit-typed *value*
        // expression in return position is a separate, still-unsupported
        // shape).
        return false;
    }

    // lowerExpr() may have moved `builder` to a different block (a
    // short-circuit return expression, e.g. `return a && rhs()`) - this
    // reads the CURRENT block fresh, never assumes it is still the block
    // this method started in, so the `ret` correctly terminates whichever
    // block is actually active now.
    if ((*result)->getType() != builder.GetInsertBlock()->getParent()->getReturnType()) {
        return false;
    }

    builder.CreateRet(*result);
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

llvm::Function* LLVMCodeGenerator::findFunction(SymbolId id) const {
    for (const auto& [fnId, function] : functions_) {
        if (fnId == id) {
            return function;
        }
    }
    return nullptr;
}

} // namespace kai::codegen
