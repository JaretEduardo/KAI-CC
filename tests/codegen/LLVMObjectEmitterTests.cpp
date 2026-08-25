#include "kai/codegen/LLVMObjectEmitter.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/codegen/LLVMCodeGenerator.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include "support/check.hpp"

#include <filesystem>
#include <sstream>
#include <string>

using kai::FileId;
using kai::SourceManager;
using kai::ast::SourceFile;
using kai::codegen::LLVMCodeGenerator;
using kai::codegen::LLVMObjectEmitter;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::ControlFlowAnalyzer;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticModel;
using kai::semantic::TypeChecker;

namespace {

// Real frontend + LLVMCodeGenerator pipeline, mirroring
// LLVMCodeGeneratorTests.cpp's own compileToLLVM() helper: this test file
// exercises LLVMObjectEmitter, but it must still start from a genuinely
// fully-checked, LLVMCodeGenerator-produced module - never a hand-built
// llvm::Module standing in for one.
bool compileToLLVM(SourceManager& sm, LLVMCodeGenerator& codegen, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();
    if (!parsed.has_value()) {
        return false;
    }

    SemanticModel model;
    SemanticAnalyzer analyzer(sm);
    model = analyzer.analyze(*parsed);

    TypeChecker checker(sm);
    checker.check(*parsed, model);

    const ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    if (!model.errors().empty()) {
        return false;
    }

    return codegen.generate(*parsed, model);
}

// NATIVE-ENTRY ADAPTATION

void testAdaptEntryPointWrapsUnitMain() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn main() {\n    print(1)\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream err;
    KAI_CHECK(LLVMObjectEmitter::adaptNativeEntryPoint(module, err));

    const llvm::Function* userMain = module.getFunction("__kai_user_main");
    KAI_CHECK(userMain != nullptr);
    if (userMain == nullptr) {
        return;
    }
    KAI_CHECK(userMain->getLinkage() == llvm::GlobalValue::InternalLinkage);
    KAI_CHECK(userMain->getReturnType()->isVoidTy());

    const llvm::Function* nativeMain = module.getFunction("main");
    KAI_CHECK(nativeMain != nullptr);
    if (nativeMain == nullptr) {
        return;
    }
    KAI_CHECK(nativeMain->arg_size() == 0);
    KAI_CHECK(nativeMain->getReturnType()->isIntegerTy(32));
    KAI_CHECK(nativeMain->getLinkage() == llvm::GlobalValue::ExternalLinkage);

    bool sawCallToUserMain = false;
    for (const llvm::BasicBlock& block : *nativeMain) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                sawCallToUserMain |= call->getCalledFunction() == userMain;
            }
        }
    }
    KAI_CHECK(sawCallToUserMain);

    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(nativeMain->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        const auto* returned = llvm::dyn_cast_or_null<llvm::ConstantInt>(ret->getReturnValue());
        KAI_CHECK(returned != nullptr);
        if (returned != nullptr) {
            KAI_CHECK(returned->getSExtValue() == 0);
        }
    }

    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));
}

void testAdaptEntryPointUsesI32MainDirectly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn main() -> i32 {\n    return 0\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream err;
    KAI_CHECK(LLVMObjectEmitter::adaptNativeEntryPoint(module, err));

    // No wrapper was created - `main` itself already matched the native
    // ABI exactly (SYNTAX.md §7).
    KAI_CHECK(module.getFunction("__kai_user_main") == nullptr);

    const llvm::Function* nativeMain = module.getFunction("main");
    KAI_CHECK(nativeMain != nullptr);
    if (nativeMain != nullptr) {
        KAI_CHECK(nativeMain->arg_size() == 0);
        KAI_CHECK(nativeMain->getReturnType()->isIntegerTy(32));
        KAI_CHECK(!nativeMain->empty()); // still has its own original body
    }

    KAI_CHECK(!llvm::verifyModule(module));
}

void testAdaptEntryPointFailsWithNoMain() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn helper() -> i64 {\n    return 1\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream err;
    KAI_CHECK(!LLVMObjectEmitter::adaptNativeEntryPoint(module, err));
    KAI_CHECK(!err.str().empty());
}

void testAdaptEntryPointFailsWithParameters() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn main(x: i64) {\n    print(x)\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream err;
    KAI_CHECK(!LLVMObjectEmitter::adaptNativeEntryPoint(module, err));
    KAI_CHECK(!err.str().empty());
}

void testAdaptEntryPointFailsWithUnsupportedReturnType() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn main() -> bool {\n    return true\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream err;
    KAI_CHECK(!LLVMObjectEmitter::adaptNativeEntryPoint(module, err));
    KAI_CHECK(!err.str().empty());
}

// OBJECT EMISSION

void testEmitProducesNonEmptyRelocatableObjectFile() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    KAI_CHECK(compileToLLVM(sm, codegen, "fn main() {\n    print(42)\n}"));

    llvm::Module& module = codegen.module();
    std::ostringstream adaptErr;
    KAI_CHECK(LLVMObjectEmitter::adaptNativeEntryPoint(module, adaptErr));

    LLVMObjectEmitter::initializeNativeTarget();

    const std::filesystem::path outputPath =
        std::filesystem::temp_directory_path() / "kai_object_emitter_test_output.o";
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);

    std::ostringstream emitErr;
    KAI_CHECK(LLVMObjectEmitter::emit(module, outputPath, emitErr));

    KAI_CHECK(std::filesystem::exists(outputPath));
    KAI_CHECK(std::filesystem::file_size(outputPath) > 0);

    // Confirm it is a real object file through LLVM's own object-file
    // reader, rather than shelling out to `file`/`objdump` (M7 spec §21).
    llvm::Expected<llvm::object::OwningBinary<llvm::object::ObjectFile>> opened =
        llvm::object::ObjectFile::createObjectFile(outputPath.string());
    KAI_CHECK(static_cast<bool>(opened));
    if (opened) {
        const llvm::object::ObjectFile& objectFile = *opened->getBinary();
        KAI_CHECK(objectFile.isRelocatableObject());
    } else {
        llvm::consumeError(opened.takeError());
    }

    std::filesystem::remove(outputPath, ignored);
}

} // namespace

int main() {
    testAdaptEntryPointWrapsUnitMain();
    testAdaptEntryPointUsesI32MainDirectly();
    testAdaptEntryPointFailsWithNoMain();
    testAdaptEntryPointFailsWithParameters();
    testAdaptEntryPointFailsWithUnsupportedReturnType();

    testEmitProducesNonEmptyRelocatableObjectFile();

    return kai::test::failureCount == 0 ? 0 : 1;
}
