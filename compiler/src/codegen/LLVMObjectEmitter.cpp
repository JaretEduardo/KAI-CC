#include "kai/codegen/LLVMObjectEmitter.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <optional>
#include <string>

namespace kai::codegen {

void LLVMObjectEmitter::initializeNativeTarget() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
}

bool LLVMObjectEmitter::adaptNativeEntryPoint(llvm::Module& module, std::ostream& err) {
    llvm::Function* userMain = module.getFunction("main");
    if (userMain == nullptr) {
        err << "kaicc: error: no 'main' function found - a native executable requires a source-level `fn main()`\n";
        return false;
    }
    if (userMain->arg_size() != 0) {
        err << "kaicc: error: 'main' must take no parameters to be used as a native executable entrypoint\n";
        return false;
    }

    llvm::Type* returnType = userMain->getReturnType();
    if (returnType->isIntegerTy(32)) {
        // Already exactly the native `int main(void)` ABI (SYNTAX.md §7) -
        // nothing to adapt.
        return true;
    }

    if (!returnType->isVoidTy()) {
        err << "kaicc: error: 'main' has an unsupported return type for a native executable entrypoint "
               "(expected no return type, or -> i32)\n";
        return false;
    }

    llvm::LLVMContext& context = module.getContext();

    // Move the KAI-level `main` out of the way of the native entry symbol.
    // Renaming (rather than replacing) keeps this the SAME llvm::Function
    // object every existing IR reference already points at - nothing else
    // needs updating. Internal linkage: it is no longer called from
    // outside this module.
    userMain->setName("__kai_user_main");
    userMain->setLinkage(llvm::GlobalValue::InternalLinkage);

    llvm::Type* i32Type = llvm::Type::getInt32Ty(context);
    llvm::FunctionType* nativeMainType = llvm::FunctionType::get(i32Type, /*isVarArg=*/false);
    llvm::Function* nativeMain =
        llvm::Function::Create(nativeMainType, llvm::GlobalValue::ExternalLinkage, "main", module);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", nativeMain);
    llvm::IRBuilder<> builder(entry);
    builder.CreateCall(userMain);
    builder.CreateRet(llvm::ConstantInt::get(i32Type, 0));

    return true;
}

bool LLVMObjectEmitter::emit(llvm::Module& module, const std::filesystem::path& outputPath, std::ostream& err) {
    const std::string targetTripleString = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple targetTriple(targetTripleString);

    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
    if (target == nullptr) {
        err << "kaicc: error: failed to look up native target '" << targetTripleString << "': " << lookupError
            << '\n';
        return false;
    }

    const llvm::TargetOptions options;
    llvm::TargetMachine* rawTargetMachine =
        target->createTargetMachine(targetTriple, llvm::sys::getHostCPUName(), /*Features=*/"", options,
                                     std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_), std::nullopt,
                                     llvm::CodeGenOptLevel::None);
    if (rawTargetMachine == nullptr) {
        err << "kaicc: error: failed to create a native TargetMachine for '" << targetTripleString << "'\n";
        return false;
    }
    const std::unique_ptr<llvm::TargetMachine> targetMachine(rawTargetMachine);

    // M7 spec §5: the module's OWN triple/DataLayout come from the actual
    // TargetMachine, never a hard-coded string.
    module.setTargetTriple(targetTriple);
    module.setDataLayout(targetMachine->createDataLayout());

    std::error_code fileError;
    llvm::raw_fd_ostream output(outputPath.string(), fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        err << "kaicc: error: failed to open '" << outputPath.string() << "' for writing: " << fileError.message()
            << '\n';
        return false;
    }

    llvm::legacy::PassManager passManager;
    if (targetMachine->addPassesToEmitFile(passManager, output, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        err << "kaicc: error: the target machine cannot emit an object file for '" << targetTripleString << "'\n";
        return false;
    }

    passManager.run(module);
    output.flush();

    if (output.has_error()) {
        err << "kaicc: error: failed while writing '" << outputPath.string() << "': " << output.error().message()
            << '\n';
        return false;
    }

    return true;
}

} // namespace kai::codegen
