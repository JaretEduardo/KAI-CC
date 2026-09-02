// Module/function orchestration and statement/block lowering. Expression
// lowering (lowerExpr() and everything it dispatches to - literals,
// identifiers, unary/binary/logical, assignment, calls) lives in the
// sibling LLVMExpressionLowering.cpp - a purely mechanical split (M4 spec
// §25) once this file passed ~700 lines: no public API change, every
// method here is still declared once, in LLVMCodeGenerator.hpp, and nothing
// was made public merely to enable the split.

#include "kai/codegen/LLVMCodeGenerator.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
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

namespace {

// KAI LANGUAGE M7B: structural-only unwrap (mirrors TypeChecker's own
// unwrapAssignmentTargetIdentifier()/unwrapAdaptableLiteral() "see
// through transparent ParenExpr only" idiom, already duplicated once
// more in LLVMExpressionLowering.cpp for the SAME reason - internal
// linkage per translation unit, never a shared header for a one-line
// structural check). Finds the ArrayLiteralExpr a `let`/`mut` array
// local's initializer names, if any - nullptr for anything else (an
// identifier, a call, ...), which generateArrayVarDeclStmt() then
// treats as the still-deliberately-unsupported "whole array value from
// somewhere other than a literal" shape (M7B spec §13).
const ast::ArrayLiteralExpr* unwrapArrayLiteralInitializer(const ast::Expr& expr) {
    if (expr.kind() == ast::ExprKind::Paren) {
        return unwrapArrayLiteralInitializer(static_cast<const ast::ParenExpr&>(expr).inner());
    }
    if (expr.kind() == ast::ExprKind::ArrayLiteral) {
        return &static_cast<const ast::ArrayLiteralExpr&>(expr);
    }
    return nullptr;
}

} // namespace

LLVMCodeGenerator::LLVMCodeGenerator(const SourceManager& sources) : sources_(sources) {}

bool LLVMCodeGenerator::generate(const ast::SourceFile& file, const SemanticModel& model) {
    module_ = std::make_unique<llvm::Module>(std::string(sources_.fileName(file.file())), context_);
    functions_.clear();
    unsupportedConstruct_.reset();

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

llvm::Module& LLVMCodeGenerator::module() {
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
    for (std::size_t i = 0; i < signature.parameterTypes.size(); ++i) {
        // KAI LANGUAGE M8B: array parameters now use lowerType()'s own
        // Array case directly, exactly like every other type - no
        // separate isArray() guard. lowerType() already produces a real
        // `[N x T]` for a supported element type, or nullptr (falling
        // into the SAME "unsupported parameter type" path below) for a
        // genuinely unsupported one (Char, a still-deferred shape) -
        // never a distinct message that would suggest array parameters
        // are a different kind of limitation. See declareFunction()'s
        // own header note and TYPE_SYSTEM.md §18 for the by-value
        // language contract this now implements (direct LLVM aggregate
        // parameter, per the M8B-approved ABI strategy - no sret/byval,
        // no hidden pointer, no reference).
        //
        // KAI LANGUAGE M10B: a bare Slice parameter IS fully supported
        // (lowerType() now lowers it - see below), but an ARRAY that
        // recursively contains a Slice anywhere in its element chain
        // (`[[i32]; 2]`, ...) is explicitly rejected HERE, before
        // lowerType() is even called - allowing it would let an M8-style
        // aggregate array parameter carry a borrowed view this
        // milestone's restricted lifetime rule was never designed to
        // track through nested storage (spec §6/§29). See
        // `isUnsupportedSliceCarryingType()`'s own doc comment for why a
        // bare Slice parameter is not caught by this check - it is meant
        // to reach lowerType() normally instead.
        if (isUnsupportedSliceCarryingType(signature.parameterTypes[i], model)) {
            recordUnsupportedConstruct("code generation is not yet supported for this parameter's type",
                                        fn.params()[i].type->span());
            return false;
        }
        llvm::Type* llvmParamType = lowerType(signature.parameterTypes[i], model);
        if (llvmParamType == nullptr) {
            // RELEASE HARDENING M2: a parameter type that never resolved
            // to a lowerable Type (references/structs/still-deferred
            // shapes - see lowerType()'s own exhaustive switch) fails
            // HERE, in Pass 1, before this function's body is ever
            // reached - e.g. `fn sum(values: &i32) -> i32` never gets far
            // enough to report its `for` loop as the culprit otherwise.
            recordUnsupportedConstruct("code generation is not yet supported for this parameter's type",
                                        fn.params()[i].type->span());
            return false;
        }
        paramTypes.push_back(llvmParamType);
    }

    // KAI LANGUAGE M8B: array RETURN values likewise now use lowerType()
    // directly - a direct LLVM aggregate return, no sret.
    //
    // KAI LANGUAGE M10B spec §5/§6: a bare Slice RETURN type was
    // unconditionally unsupported here too (not just an array recursively
    // containing one), since KAI had no lifetime/provenance analysis yet
    // to make returning a borrowed view sound.
    //
    // KAI LANGUAGE M11B: now that KAI LANGUAGE M11A's own restricted,
    // flow-sensitive provenance analysis semantically rejects every
    // Slice-returning `return` expression EXCEPT one already proven
    // `External` (backed by storage that genuinely outlives this
    // function's own invocation), a bare Slice return that reaches this
    // point in codegen has ALREADY passed that check - TypeChecker never
    // let an unsafe one through. This uses the SAME
    // `isUnsupportedSliceCarryingType()` policy the parameter loop above
    // and generateVarDeclStmt()'s own local check use, so only an ARRAY
    // that recursively contains a Slice remains rejected here - never a
    // bare Slice return itself. This is a codegen ABI-support decision
    // only; it does not re-derive or duplicate M11A's own provenance
    // analysis in any way (a Local/Unknown Slice return never reaches
    // codegen at all - it already failed at TypeChecker with
    // EscapingLocalSlice).
    if (isUnsupportedSliceCarryingType(signature.returnType, model)) {
        const SourceSpan span = fn.returnType() != nullptr ? fn.returnType()->span() : fn.name().span;
        recordUnsupportedConstruct("code generation is not yet supported for this function's return type", span);
        return false;
    }
    llvm::Type* returnType = lowerType(signature.returnType, model);
    if (returnType == nullptr) {
        const SourceSpan span = fn.returnType() != nullptr ? fn.returnType()->span() : fn.name().span;
        recordUnsupportedConstruct("code generation is not yet supported for this function's return type", span);
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

    const StatementResult bodyResult = generateBlock(fn.body(), *function, builder, model);
    if (bodyResult == StatementResult::Failed) {
        return false;
    }

    // Function fallthrough policy (M4 spec §22/§23, extended M5 §16): if
    // the body's last-lowered statement returned StatementResult::
    // Terminated (e.g. an if/else whose every arm returns), every path
    // already ends in an explicit `return` - nothing more to do. Only
    // FallsThrough needs this policy at all, and `builder`'s current block
    // is read fresh here - never assumed to still be `entry` - since a
    // short-circuit expression or nested if/while in the last statement
    // may have left it somewhere else entirely.
    if (bodyResult == StatementResult::FallsThrough) {
        if (function->getReturnType()->isVoidTy()) {
            // Not inventing KAI semantics: a Unit function is already
            // allowed to fall through by frontend semantics - LLVM merely
            // requires every defined block to terminate.
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

LLVMCodeGenerator::StatementResult LLVMCodeGenerator::generateBlock(const ast::BlockStmt& block,
                                                                     llvm::Function& function,
                                                                     llvm::IRBuilder<>& builder,
                                                                     const SemanticModel& model) {
    StatementResult result = StatementResult::FallsThrough;
    for (const auto& stmt : block.statements()) {
        result = generateStatement(*stmt, function, builder, model);
        if (result == StatementResult::Failed) {
            return StatementResult::Failed;
        }
        if (result == StatementResult::Terminated) {
            // This path already ended in an LLVM terminator (a ReturnStmt,
            // or an if/else whose every arm returns). The frontend still
            // fully checked every statement that follows (ControlFlow-
            // Analyzer's own "no unreachable-code analysis" stance) - this
            // pass simply stops LOWERING them here rather than append
            // instructions after a terminator, which would be invalid IR.
            break;
        }
    }
    return result;
}

// No `default:` case: StmtKind is fully implemented today, mirroring
// TypeChecker.cpp's/ControlFlowAnalyzer.cpp's own exhaustive switch over
// it. ForStmt lowering (KAI LANGUAGE M6, post-alpha.2) is real - see
// generateForStmt().
LLVMCodeGenerator::StatementResult LLVMCodeGenerator::generateStatement(const ast::Stmt& stmt,
                                                                         llvm::Function& function,
                                                                         llvm::IRBuilder<>& builder,
                                                                         const SemanticModel& model) {
    switch (stmt.kind()) {
        case ast::StmtKind::Block:
            // Recurses into the SAME BasicBlock/builder - no new block is
            // created merely for lexical scoping. Note: KAI 0.1's current
            // grammar has no standalone `{ ... }` statement production
            // (parseStatement() only reaches parseBlock() via
            // fn/if/while/for) - this case exists for StmtKind switch-
            // exhaustiveness and forward compatibility, not because it is
            // reachable from real source text today.
            return generateBlock(static_cast<const ast::BlockStmt&>(stmt), function, builder, model);

        case ast::StmtKind::VarDecl:
            return generateVarDeclStmt(static_cast<const ast::VarDeclStmt&>(stmt), function, builder, model)
                       ? StatementResult::FallsThrough
                       : StatementResult::Failed;

        case ast::StmtKind::Expr:
            return generateExprStmt(static_cast<const ast::ExprStmt&>(stmt), builder, model)
                       ? StatementResult::FallsThrough
                       : StatementResult::Failed;

        case ast::StmtKind::Return:
            return generateReturnStmt(static_cast<const ast::ReturnStmt&>(stmt), builder, model)
                       ? StatementResult::Terminated
                       : StatementResult::Failed;

        case ast::StmtKind::If:
            return generateIfStmt(static_cast<const ast::IfStmt&>(stmt), 0, function, builder, model);

        case ast::StmtKind::While:
            return generateWhileStmt(static_cast<const ast::WhileStmt&>(stmt), function, builder, model);

        case ast::StmtKind::For:
            return generateForStmt(static_cast<const ast::ForStmt&>(stmt), function, builder, model);
    }
    return StatementResult::Failed;
}

void LLVMCodeGenerator::recordUnsupportedConstruct(std::string description, SourceSpan span) {
    if (!unsupportedConstruct_.has_value()) {
        unsupportedConstruct_ = UnsupportedConstruct{std::move(description), span};
    }
}

LLVMCodeGenerator::StatementResult LLVMCodeGenerator::generateIfStmt(const ast::IfStmt& stmt, std::size_t branchIndex,
                                                                      llvm::Function& function,
                                                                      llvm::IRBuilder<>& builder,
                                                                      const SemanticModel& model) {
    const ast::IfBranch& branch = stmt.branches()[branchIndex];
    const bool isLastBranch = branchIndex + 1 == stmt.branches().size();
    // An `else if` (branchIndex + 1 exists) or a final `else` both count
    // as "this branch has an else" - the only difference is what lowers
    // into that else block (see below).
    const bool hasElse = !isLastBranch || stmt.elseClause().has_value();

    const std::optional<llvm::Value*> condition = lowerExpr(*branch.condition, model, builder);
    if (!condition.has_value() || *condition == nullptr) {
        return StatementResult::Failed;
    }
    if (!(*condition)->getType()->isIntegerTy(1)) {
        // Defensive only: the frontend already guarantees a Bool
        // condition. Never truthiness/integer reinterpretation.
        return StatementResult::Failed;
    }
    // lowerExpr() may have moved `builder` to a different block (a
    // short-circuit `&&`/`||` condition creates its own blocks - see
    // lowerLogicalExpr()) - the CondBr below must originate from wherever
    // it ACTUALLY left `builder`, never the block current before lowering
    // the condition.
    llvm::BasicBlock* conditionEndBlock = builder.GetInsertBlock();

    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(context_, "if.then", &function);
    llvm::BasicBlock* elseBlock = hasElse ? llvm::BasicBlock::Create(context_, "if.else", &function) : nullptr;
    // Created eagerly and erased later if it turns out to be unreachable
    // (both arms terminate) - simpler than deferring block creation until
    // that's known, and an unreferenced, un-inserted-into block costs
    // nothing to discard.
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context_, "if.end", &function);

    builder.SetInsertPoint(conditionEndBlock);
    builder.CreateCondBr(*condition, thenBlock, hasElse ? elseBlock : mergeBlock);

    builder.SetInsertPoint(thenBlock);
    const StatementResult thenResult = generateBlock(*branch.body, function, builder, model);
    if (thenResult == StatementResult::Failed) {
        return StatementResult::Failed;
    }
    const bool thenFallsThrough = thenResult == StatementResult::FallsThrough;
    if (thenFallsThrough) {
        // Branch from the THEN body's ACTUAL final block (generateBlock()
        // itself may have left `builder` somewhere other than `thenBlock`
        // - a nested if/while, or a short-circuit expression, moves it).
        builder.CreateBr(mergeBlock);
    }

    // No `else` at all: the false-condition edge (built above) already
    // targets `mergeBlock` directly, so it is unconditionally reachable
    // regardless of what the `then` arm does - a plain `if` with no
    // `else` therefore always falls through (M5 spec §5), matching
    // ControlFlowAnalyzer::analyzeIfStmt()'s own "no else -> FallsThrough,
    // unconditionally" rule.
    bool elseFallsThrough = !hasElse;
    if (hasElse) {
        builder.SetInsertPoint(elseBlock);
        // `else if` lowers AS a nested if (recursing into the next
        // branch) - `else` lowers its block directly. Both are just
        // "what populates the else block" and compose identically with
        // the merge/termination logic below; no source-spelling special
        // case exists anywhere in this method.
        const StatementResult elseResult = isLastBranch
                                                ? generateBlock(*stmt.elseClause()->body, function, builder, model)
                                                : generateIfStmt(stmt, branchIndex + 1, function, builder, model);
        if (elseResult == StatementResult::Failed) {
            return StatementResult::Failed;
        }
        elseFallsThrough = elseResult == StatementResult::FallsThrough;
        if (elseFallsThrough) {
            // Same reasoning as the `then` branch above: branch from
            // whichever block the else-side lowering ACTUALLY left
            // current, not `elseBlock` itself.
            builder.CreateBr(mergeBlock);
        }
    }

    if (!thenFallsThrough && !elseFallsThrough) {
        // Both arms terminated (M5 spec §4/§6 case D): `mergeBlock` has no
        // predecessors and is therefore unreachable - erase it rather
        // than leave an empty, unterminated block for the verifier to
        // reject, or fabricate an instruction merely to satisfy it.
        // Propagate "no continuation on this path" to the caller exactly
        // like ControlFlowAnalyzer::FlowResult::AlwaysReturns.
        mergeBlock->eraseFromParent();
        return StatementResult::Terminated;
    }

    builder.SetInsertPoint(mergeBlock);
    return StatementResult::FallsThrough;
}

LLVMCodeGenerator::StatementResult LLVMCodeGenerator::generateWhileStmt(const ast::WhileStmt& stmt,
                                                                         llvm::Function& function,
                                                                         llvm::IRBuilder<>& builder,
                                                                         const SemanticModel& model) {
    llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context_, "while.cond", &function);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "while.body", &function);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(context_, "while.end", &function);

    // Enter the loop from wherever `builder` currently is (the
    // preheader) - never assumed to be the function entry block.
    builder.CreateBr(conditionBlock);

    builder.SetInsertPoint(conditionBlock);
    const std::optional<llvm::Value*> condition = lowerExpr(stmt.condition(), model, builder);
    if (!condition.has_value() || *condition == nullptr) {
        return StatementResult::Failed;
    }
    if (!(*condition)->getType()->isIntegerTy(1)) {
        return StatementResult::Failed;
    }
    // Same reasoning as generateIfStmt(): a short-circuit `&&`/`||`
    // condition may have moved `builder` away from `conditionBlock` - the
    // CondBr must originate from wherever it ACTUALLY ended up.
    llvm::BasicBlock* conditionEndBlock = builder.GetInsertBlock();
    builder.SetInsertPoint(conditionEndBlock);
    builder.CreateCondBr(*condition, bodyBlock, exitBlock);

    builder.SetInsertPoint(bodyBlock);
    const StatementResult bodyResult = generateBlock(stmt.body(), function, builder, model);
    if (bodyResult == StatementResult::Failed) {
        return StatementResult::Failed;
    }
    if (bodyResult == StatementResult::FallsThrough) {
        // Back-edge from the body's ACTUAL final block, never assumed to
        // still be `bodyBlock` itself.
        builder.CreateBr(conditionBlock);
    }
    // Terminated: the body always returns on this path - no back-edge, no
    // instruction after the terminator it already ended in.

    builder.SetInsertPoint(exitBlock);
    // A `while` loop can never be soundly proven to execute even once
    // without constant-condition reasoning (which neither the frontend's
    // ControlFlowAnalyzer nor this backend performs) - the condition's
    // false edge always targets `exitBlock` directly, so WhileStmt as a
    // whole ALWAYS falls through, regardless of the body's own result.
    // Mirrors ControlFlowAnalyzer::analyzeStatement()'s own documented
    // "conservatively FallsThrough, unconditionally" stance for While.
    return StatementResult::FallsThrough;
}

// KAI LANGUAGE M6 (`for` + integer ranges, post-alpha.2): see this
// method's own header doc comment for the full conceptual lowering.
// TypeChecker already guarantees `stmt.iterable()` is a
// BinaryExpr{Range} whose two endpoints share one lowerable concrete
// integer Type (checkForStmt()/checkIntegerRangeFor() in
// TypeChecker.cpp) - the checks below are DEFENSIVE re-verification
// (mirrors generateIfStmt()'s/generateWhileStmt()'s own "defensive only"
// condition-type checks), not a second independent type analysis.
LLVMCodeGenerator::StatementResult LLVMCodeGenerator::generateForStmt(const ast::ForStmt& stmt,
                                                                       llvm::Function& function,
                                                                       llvm::IRBuilder<>& builder,
                                                                       const SemanticModel& model) {
    if (stmt.iterable().kind() != ast::ExprKind::Binary) {
        recordUnsupportedConstruct("code generation is not yet supported for this 'for' loop's iterable",
                                    stmt.iterable().span());
        return StatementResult::Failed;
    }
    const auto& range = static_cast<const ast::BinaryExpr&>(stmt.iterable());
    if (range.op() != ast::BinaryOperator::Range) {
        recordUnsupportedConstruct("code generation is not yet supported for this 'for' loop's iterable",
                                    stmt.iterable().span());
        return StatementResult::Failed;
    }

    const std::optional<SymbolId> loopVarId = model.declarationSymbol(stmt.variable());
    assert(loopVarId.has_value());
    const Symbol& loopVarSymbol = model.symbol(*loopVarId);

    llvm::Type* elementType = lowerType(loopVarSymbol.type, model);
    if (elementType == nullptr || !elementType->isIntegerTy()) {
        // Defensive only - TypeChecker's isIntegerDomain restriction
        // already guarantees a concrete i8/i16/i32/i64/u8/u16/u32/u64
        // element type reaches here.
        recordUnsupportedConstruct("code generation is not yet supported for this 'for' loop's element type",
                                    stmt.iterable().span());
        return StatementResult::Failed;
    }
    const bool isSigned = loopVarSymbol.type.isSignedInteger();

    // `start`/`end` are each evaluated EXACTLY ONCE, here in the
    // preheader - never re-lowered inside the loop (M6 spec #1/#2/#12).
    const std::optional<llvm::Value*> startValue = lowerExpr(range.left(), model, builder);
    if (!startValue.has_value() || *startValue == nullptr) {
        return StatementResult::Failed;
    }
    const std::optional<llvm::Value*> endValue = lowerExpr(range.right(), model, builder);
    if (!endValue.has_value() || *endValue == nullptr) {
        return StatementResult::Failed;
    }
    if ((*startValue)->getType() != elementType || (*endValue)->getType() != elementType) {
        return StatementResult::Failed;
    }

    // The induction variable's own storage IS the loop variable's
    // storage - registered in `locals_` under the loop variable's own
    // SymbolId below, so the body's ordinary IdentifierExpr loads (via
    // lowerIdentifierExpr()/findLocalSlot()) transparently see the
    // current induction value with no separate PHI/copy mechanism. No
    // assignment can ever target it (TypeChecker rejects that via
    // AssignmentToImmutableBinding, since SemanticAnalyzer declared this
    // Symbol immutable), so only this method itself ever stores to it.
    llvm::AllocaInst* induction =
        createEntryBlockAlloca(function, elementType, sources_.text(stmt.variable().span));
    builder.CreateStore(*startValue, induction);
    locals_.emplace_back(*loopVarId, induction);

    llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context_, "for.cond", &function);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "for.body", &function);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(context_, "for.end", &function);

    // Enter the loop from wherever `builder` currently is (the
    // preheader, where `startValue`/`endValue` were just lowered) -
    // never assumed to be the function entry block.
    builder.CreateBr(conditionBlock);

    builder.SetInsertPoint(conditionBlock);
    llvm::Value* currentValue = builder.CreateLoad(elementType, induction);
    // Half-open range: `i < end` alone decides continuation, so
    // `start >= end` naturally yields zero iterations with no separate
    // pre-check, and `induction` is never incremented past `end` (its
    // final stored value is always exactly `end`, which is by
    // construction representable in `elementType` - no wraparound risk
    // at the type's maximum, e.g. `for i in 250..255` with `u8`).
    llvm::Value* condition =
        isSigned ? builder.CreateICmpSLT(currentValue, *endValue) : builder.CreateICmpULT(currentValue, *endValue);
    builder.CreateCondBr(condition, bodyBlock, exitBlock);

    builder.SetInsertPoint(bodyBlock);
    const StatementResult bodyResult = generateBlock(stmt.body(), function, builder, model);
    if (bodyResult == StatementResult::Failed) {
        return StatementResult::Failed;
    }
    if (bodyResult == StatementResult::FallsThrough) {
        // Reload rather than reuse `currentValue`: the body may contain
        // arbitrary statements (a nested for/while, calls, ...) between
        // entering `bodyBlock` and here - `currentValue` is still valid
        // LLVM SSA-wise (it dominates this point), but reloading matches
        // this class's established memory-based, no-PHI style (see
        // generateWhileStmt()) and stays correct even if a future
        // milestone ever lets the body store to `induction` itself.
        llvm::Value* bodyEndValue = builder.CreateLoad(elementType, induction);
        llvm::Value* next = builder.CreateAdd(bodyEndValue, llvm::ConstantInt::get(elementType, 1));
        builder.CreateStore(next, induction);
        // Back-edge from the body's ACTUAL final block, never assumed to
        // still be `bodyBlock` itself.
        builder.CreateBr(conditionBlock);
    }
    // Terminated: the body always returns on this path - no back-edge, no
    // instruction after the terminator it already ended in.

    builder.SetInsertPoint(exitBlock);
    // Same conservative stance as generateWhileStmt(): a `for` loop can
    // never be soundly proven to execute even once without constant-
    // range reasoning (which neither the frontend's ControlFlowAnalyzer
    // nor this backend performs) - the condition's false edge always
    // targets `exitBlock` directly, so ForStmt as a whole ALWAYS falls
    // through, regardless of the body's own result.
    return StatementResult::FallsThrough;
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

    // KAI LANGUAGE M10B spec §6/§29: a bare Slice LOCAL is fully
    // supported (falls through to the generic path below, once
    // lowerType() gives it a real Type just like any other), but an
    // ARRAY local that recursively contains a Slice anywhere in its
    // nested element chain is not - see declareFunction()'s own
    // identical guard for the full rationale (an M8-style array
    // aggregate must never silently carry a borrowed view, whether
    // through a function boundary or through plain local storage). This
    // is checked BEFORE lowerType() is even called, mirroring
    // declareFunction()'s own ordering.
    if (isUnsupportedSliceCarryingType(symbol.type, model)) {
        return false;
    }

    llvm::Type* llvmType = lowerType(symbol.type, model);
    if (llvmType == nullptr || llvmType->isVoidTy()) {
        // nullptr: Error/Unresolved/Char, same as everywhere else.
        // isVoidTy(): a Unit-typed local - LLVM has no storable void
        // value, so this is an explicit, documented policy failure, not
        // a fallthrough of the Error/Unresolved case (lowerType()
        // legitimately returns void for Unit, since Unit IS a valid
        // function return type - only a LOCAL's storage type rejects it).
        return false;
    }

    if (llvm::ArrayType* arrayType = llvm::dyn_cast<llvm::ArrayType>(llvmType)) {
        // KAI LANGUAGE M7B: an array-typed local needs its own
        // initialization path - an ArrayLiteralExpr is not (and stays
        // not) a general lowerExpr()-producible SSA value (see
        // lowerExpr()'s own ArrayLiteral case, still nullopt); it is
        // only ever lowered by evaluating each element directly into the
        // local's own storage, in source order. See
        // generateArrayVarDeclStmt()'s own doc comment for exactly what
        // initializer shape is (and, per M7B spec §13, deliberately is
        // NOT) supported.
        return generateArrayVarDeclStmt(varDecl, *id, arrayType, function, builder, model);
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

// KAI LANGUAGE M7B/M8B: an array-typed local's own initialization path.
// Two supported initializer shapes:
//
//   1. A (possibly paren-wrapped) ArrayLiteralExpr directly -
//      `let xs = [10, 20, 30]` / `mut xs = [10, 20, 30]` - the fast path,
//      unchanged since M7B: elements are lowered straight into this
//      local's own storage via lowerArrayLiteralIntoStorage(), never
//      through an intermediate aggregate SSA value/temporary.
//   2. KAI LANGUAGE M8B: any OTHER initializer shape that evaluates to an
//      array-typed value - `let b = a` (an identifier), a call result, a
//      parenthesized expression, ... - now falls back to the same
//      generic lowerExpr()-then-store path generateVarDeclStmt() already
//      uses for every non-array type, since ArrayLiteralExpr becoming a
//      general lowerExpr()-producible value (M8B's lowerArrayLiteralExpr)
//      means that path is already fully array-agnostic. This is exactly
//      KAI LANGUAGE M8A's approved LANGUAGE semantics (ordinary
//      value-copy, no aliasing - TYPE_SYSTEM.md §19), not something
//      broadened silently: the fast path above is kept because it is
//      strictly more efficient (no temporary, no extra load) and remains
//      correct, not because the fallback couldn't also handle a literal.
// See lowerAssignmentExpr()'s own identical two-path shape for `a = b`
// (LLVMExpressionLowering.cpp).
bool LLVMCodeGenerator::generateArrayVarDeclStmt(const ast::VarDeclStmt& varDecl, SymbolId id,
                                                  llvm::ArrayType* arrayType, llvm::Function& function,
                                                  llvm::IRBuilder<>& builder, const SemanticModel& model) {
    const ast::ArrayLiteralExpr* literal = unwrapArrayLiteralInitializer(varDecl.initializer());
    if (literal != nullptr) {
        // The alloca is created FIRST (entry block, per this class's
        // existing local-storage convention) and bound into `locals_`
        // only AFTER its contents are successfully initialized below - so
        // a failed initialization never leaves a half-bound local
        // reachable by any later statement.
        llvm::AllocaInst* slot = createEntryBlockAlloca(function, arrayType, sources_.text(varDecl.name().span));
        if (!lowerArrayLiteralIntoStorage(*literal, slot, arrayType, model, builder)) {
            return false;
        }

        locals_.emplace_back(id, slot);
        return true;
    }

    // KAI LANGUAGE M8B: generic fallback - `let b = a` and friends.
    // Mirrors generateVarDeclStmt()'s own scalar path exactly (lower the
    // initializer, verify its LLVM type matches, allocate, store, bind) -
    // no array-specific logic beyond the isArrayTy() dispatch that
    // already routed us here.
    const std::optional<llvm::Value*> initializer = lowerExpr(varDecl.initializer(), model, builder);
    if (!initializer.has_value() || *initializer == nullptr) {
        return false;
    }
    if ((*initializer)->getType() != arrayType) {
        return false;
    }

    llvm::AllocaInst* slot = createEntryBlockAlloca(function, arrayType, sources_.text(varDecl.name().span));
    builder.CreateStore(*initializer, slot);
    locals_.emplace_back(id, slot);
    return true;
}

// KAI LANGUAGE M7B: stores an ArrayLiteralExpr's elements directly into
// `storage` (an already-allocated `arrayType`-typed address - an
// AllocaInst for a top-level local, or a GEP'd sub-address for a nested
// array literal element - see the recursive case below), left to right,
// EXACTLY ONCE each (M7B spec §9/§12): every element is lowered via
// exactly one lowerExpr()/recursive call, in source order, with no
// element re-evaluated. A nested ArrayLiteralExpr element (e.g.
// `[[1, 2], [3, 4]]`) recurses into this SAME function against a GEP'd
// sub-address, rather than first materializing an inner aggregate SSA
// value and storing that - this is what lets nested array literals work
// through the identical mechanism with no separate "aggregate value
// construction" path (M7B spec §1: multidimensional arrays are in scope
// exactly when recursive lowering "genuinely works without broadening
// the implementation" - this is that case). Returns false (leaving
// `storage` partially initialized, but never read - the caller always
// discards it on failure) the moment any element fails to lower or its
// LLVM type disagrees with the array's own element type.
bool LLVMCodeGenerator::lowerArrayLiteralIntoStorage(const ast::ArrayLiteralExpr& array, llvm::Value* storage,
                                                      llvm::ArrayType* arrayType, const SemanticModel& model,
                                                      llvm::IRBuilder<>& builder) {
    llvm::Type* elementType = arrayType->getElementType();
    llvm::Type* indexType = llvm::Type::getInt32Ty(context_);
    llvm::Value* zero = llvm::ConstantInt::get(indexType, 0);

    const std::vector<ast::ExprPtr>& elements = array.elements();
    for (std::size_t i = 0; i < elements.size(); ++i) {
        llvm::Value* elementAddress = builder.CreateGEP(
            arrayType, storage, {zero, llvm::ConstantInt::get(indexType, i)}, "array.literal.elem");

        if (elements[i]->kind() == ast::ExprKind::ArrayLiteral) {
            auto* nestedArrayType = llvm::dyn_cast<llvm::ArrayType>(elementType);
            if (nestedArrayType == nullptr) {
                return false;
            }
            if (!lowerArrayLiteralIntoStorage(static_cast<const ast::ArrayLiteralExpr&>(*elements[i]),
                                               elementAddress, nestedArrayType, model, builder)) {
                return false;
            }
            continue;
        }

        const std::optional<llvm::Value*> value = lowerExpr(*elements[i], model, builder);
        if (!value.has_value() || *value == nullptr) {
            return false;
        }
        if ((*value)->getType() != elementType) {
            return false;
        }
        // lowerExpr() may have moved `builder` (a short-circuit element
        // expression) - the store below always goes through `builder` at
        // its now-current position, same discipline as every other
        // lowering in this class.
        builder.CreateStore(*value, elementAddress);
    }
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
llvm::Type* LLVMCodeGenerator::lowerType(Type type, const SemanticModel& model) {
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
        case TypeKind::Str:
            // Minimal String Literal Support milestone: an immutable
            // { ptr, i64 } pointer+length descriptor (see Type::str()'s
            // own comment) - opaque LLVM 22 pointer to the byte data
            // (never element-typed), plus the decoded byte length. This
            // is a plain LLVM struct value: alloca/store/load through the
            // existing generic local-variable machinery works unmodified
            // once this case exists (see generateVarDeclStmt()).
            return llvm::StructType::get(context_,
                                          {llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_)});
        case TypeKind::Unresolved:
        case TypeKind::Error:
        case TypeKind::Char:
            return nullptr;
        case TypeKind::Slice:
            // KAI LANGUAGE M10B: a real `{ptr, i64}` aggregate - an
            // opaque data pointer to the first element (never element-
            // typed, exactly like Str's own pointer field) plus a
            // runtime element COUNT (never a byte length, unlike Str's
            // otherwise-identical-looking field). Internal backend
            // representation only - never a promised stable external C
            // ABI, never source-visible struct syntax (TYPE_SYSTEM.md's
            // own "Slices" section). This makes lowerType() fully
            // permissive for Slice, including when nested inside an
            // Array below - the language-level restriction against an
            // EXECUTABLE array/parameter/return/local recursively
            // containing a Slice is enforced separately, at the specific
            // call sites that matter (see typeContainsSlice()'s own doc
            // comment in the header), never inside this function.
            return llvm::StructType::get(context_,
                                          {llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_)});
        case TypeKind::Array: {
            // KAI LANGUAGE M7B: a real `[N x ElementType]` LLVM array
            // type - driven entirely by whatever this SAME lowerType()
            // can already lower for the element type (see this method's
            // own header doc comment). A still-unsupported element type
            // (Char, or a still-deferred shape) makes the ARRAY itself
            // unsupported too, via the identical nullptr signal every
            // other unsupported Type already returns - never a crash.
            llvm::Type* elementType = lowerType(model.arrayElementType(type), model);
            if (elementType == nullptr) {
                return nullptr;
            }
            return llvm::ArrayType::get(elementType, model.arrayLength(type));
        }
    }
    return nullptr;
}

bool LLVMCodeGenerator::typeContainsSlice(Type type, const SemanticModel& model) const {
    if (type.isSlice()) {
        return true;
    }
    if (type.isArray()) {
        return typeContainsSlice(model.arrayElementType(type), model);
    }
    return false;
}

bool LLVMCodeGenerator::isUnsupportedSliceCarryingType(Type type, const SemanticModel& model) const {
    // KAI LANGUAGE M11B: a bare Slice is never itself unsupported by this
    // policy - only an Array wrapping one is. `typeContainsSlice(type)`
    // is still `true` for a bare Slice, so the `isArray()` conjunct is
    // what excludes it here, exactly like the parameter check already
    // did before M11B (this now just gives that same shape a name and
    // reuses it for the return-type check too, which previously called
    // typeContainsSlice() directly with no such conjunct).
    return type.isArray() && typeContainsSlice(type, model);
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
