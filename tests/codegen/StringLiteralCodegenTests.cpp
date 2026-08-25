// Minimal String Literal Support milestone: focused LLVM codegen coverage
// for the new Str descriptor/global/runtime-call lowering (see Type.hpp's
// Type::str() and LLVMCodeGenerator's lowerType()/lowerLiteralExpr()/
// lowerPrintCall()). Kept as its own small file rather than growing the
// already-2600+-line LLVMCodeGeneratorTests.cpp (M8 spec §8/§21).
//
// Mirrors LLVMCodeGeneratorTests.cpp's own compileToLLVM() pipeline
// helper (duplicated per-file in this directory - see
// LLVMObjectEmitterTests.cpp for the same, existing convention) rather
// than sharing a header, since no such shared codegen test header exists
// yet in this repository.

#include "kai/codegen/LLVMCodeGenerator.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "support/check.hpp"

#include <string>
#include <utility>

using kai::FileId;
using kai::SourceManager;
using kai::ast::SourceFile;
using kai::codegen::LLVMCodeGenerator;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticModel;
using kai::semantic::TypeChecker;

namespace {

struct Generated {
    ParseResult<SourceFile> parsed;
    SemanticModel model;
    bool generationSucceeded = false;
};

Generated compileToLLVM(SourceManager& sm, LLVMCodeGenerator& codegen, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();

    SemanticModel model;
    bool generationSucceeded = false;
    if (parsed.has_value()) {
        SemanticAnalyzer analyzer(sm);
        model = analyzer.analyze(*parsed);

        TypeChecker checker(sm);
        checker.check(*parsed, model);

        const ControlFlowAnalyzer flow;
        flow.check(*parsed, model);

        if (model.errors().empty()) {
            generationSucceeded = codegen.generate(*parsed, model);
        }
    }

    return Generated{std::move(parsed), std::move(model), generationSucceeded};
}

// Finds the one GlobalVariable this test expects a string literal to have
// produced - deliberately tolerant of exact naming (`kai.str`, `kai.str.1`,
// ...; M8 spec §10 explicitly allows separate, non-interned constants per
// literal) by matching on shape (private, constant, backed by a
// ConstantDataArray/ConstantAggregateZero of i8) rather than name.
const llvm::GlobalVariable* findStringDataGlobal(const llvm::Module& module) {
    for (const llvm::GlobalVariable& global : module.globals()) {
        if (!global.isConstant() || global.getLinkage() != llvm::GlobalValue::PrivateLinkage) {
            continue;
        }
        if (const auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(global.getValueType())) {
            if (arrayType->getElementType()->isIntegerTy(8)) {
                return &global;
            }
        }
    }
    return nullptr;
}

// TEST: a string literal lowers to a private, internal-linkage,
// read-only global holding the EXACT decoded bytes, with no heap
// allocation and no mutable data (M8 spec §9).
void testStringLiteralCreatesGlobalConstant() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(\"Hello, KAI!\")\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::GlobalVariable* global = findStringDataGlobal(codegen.module());
    KAI_CHECK(global != nullptr);
    if (global == nullptr) {
        return;
    }
    KAI_CHECK(global->isConstant());
    KAI_CHECK(global->getLinkage() == llvm::GlobalValue::PrivateLinkage);

    const auto* arrayType = llvm::cast<llvm::ArrayType>(global->getValueType());
    KAI_CHECK(arrayType->getNumElements() == 11); // "Hello, KAI!" - 11 bytes, no escapes

    const auto* data = llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
    KAI_CHECK(data != nullptr);
    if (data != nullptr) {
        KAI_CHECK(data->getAsString() == "Hello, KAI!");
    }
}

// TEST: print(<Str>) declares and calls the length-explicit runtime ABI
// entry point, never a NUL-terminated one.
void testPrintStringEmitsRuntimeDeclarationAndCall() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(\"hi\")\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* printStrFn = codegen.module().getFunction("kai_print_str");
    KAI_CHECK(printStrFn != nullptr);
    if (printStrFn == nullptr) {
        return;
    }
    KAI_CHECK(printStrFn->arg_size() == 2);
    KAI_CHECK(printStrFn->getReturnType()->isVoidTy());

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    int callCount = 0;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                callCount += (call->getCalledFunction() == printStrFn) ? 1 : 0;
            }
        }
    }
    KAI_CHECK(callCount == 1);
}

// TEST: `let message = "..."` then `print(message)` - the Str descriptor
// goes through the SAME generic alloca/store/load local machinery every
// other type already uses (no string-specific assignment/storage path
// was added - M8 spec §12).
void testInferredStringLocalStoreLoadAndPrint() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    let message = \"Welcome to KAI\"\n    print(message)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr || mainFn->empty()) {
        return;
    }

    bool sawStructAlloca = false;
    bool sawStore = false;
    bool sawLoad = false;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            sawStructAlloca |= alloca->getAllocatedType()->isStructTy();
        }
        sawStore |= llvm::isa<llvm::StoreInst>(inst);
        sawLoad |= llvm::isa<llvm::LoadInst>(inst);
    }
    KAI_CHECK(sawStructAlloca);
    KAI_CHECK(sawStore);
    KAI_CHECK(sawLoad);

    KAI_CHECK(codegen.module().getFunction("kai_print_str") != nullptr);
}

// TEST (mut reassignment - M8 spec §12, optional but naturally
// supported): reassigning a `mut` Str local goes through the same
// generic lowerAssignmentExpr() every other type already uses.
void testMutStringReassignment() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn main() {\n    mut message = \"first\"\n    message = \"second\"\n    print(message)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    int storeCount = 0;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        storeCount += llvm::isa<llvm::StoreInst>(inst) ? 1 : 0;
    }
    KAI_CHECK(storeCount == 2); // initializer store + reassignment store
}

// REQUIRED regression (M8 spec #14): an embedded \0 escape must not
// affect the global's declared byte length or truncate its data - the
// array has exactly 3 elements ('a', 0x00, 'b'), and the runtime call's
// length argument is the ConstantInt 3, never derived from strlen.
void testEmbeddedNulDoesNotBreakGlobalLengthOrIR() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(\"a\\0b\")\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::GlobalVariable* global = findStringDataGlobal(codegen.module());
    KAI_CHECK(global != nullptr);
    if (global == nullptr) {
        return;
    }
    const auto* arrayType = llvm::cast<llvm::ArrayType>(global->getValueType());
    KAI_CHECK(arrayType->getNumElements() == 3);

    const auto* data = llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
    KAI_CHECK(data != nullptr);
    if (data != nullptr) {
        const llvm::StringRef raw = data->getRawDataValues();
        KAI_CHECK(raw.size() == 3);
        KAI_CHECK(raw[0] == 'a');
        KAI_CHECK(raw[1] == '\0');
        KAI_CHECK(raw[2] == 'b');
    }

    // The kai_print_str call's length argument is the literal ConstantInt
    // 3 - never anything strlen-shaped (which would see just "a").
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawLengthThree = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "kai_print_str") {
                    const auto* lengthArg = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
                    sawLengthThree = lengthArg != nullptr && lengthArg->getZExtValue() == 3;
                }
            }
        }
    }
    KAI_CHECK(sawLengthThree);
}

} // namespace

int main() {
    testStringLiteralCreatesGlobalConstant();
    testPrintStringEmitsRuntimeDeclarationAndCall();
    testInferredStringLocalStoreLoadAndPrint();
    testMutStringReassignment();
    testEmbeddedNulDoesNotBreakGlobalLengthOrIR();

    return kai::test::failureCount == 0 ? 0 : 1;
}
