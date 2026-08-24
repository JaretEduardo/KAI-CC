#include "kai/codegen/LLVMCodeGenerator.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "support/check.hpp"

#include <iostream>
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

// Real frontend pipeline: SourceManager -> Parser -> SemanticAnalyzer ->
// TypeChecker -> ControlFlowAnalyzer -> LLVMCodeGenerator. No fake
// pre-typed AST is constructed for convenience - LLVMCodeGenerator is
// only ever exercised on a genuinely fully-checked SemanticModel, exactly
// as it is documented to require.
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

// TEST 1: fn main() -> i64 { return 42 }
void testMainReturnsI64Literal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 42\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();

    llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }

    KAI_CHECK(mainFn->arg_size() == 0);
    KAI_CHECK(mainFn->getReturnType()->isIntegerTy(64));
    KAI_CHECK(!mainFn->empty());
    if (mainFn->empty()) {
        return;
    }

    const llvm::BasicBlock& entry = mainFn->getEntryBlock();
    const llvm::Instruction* terminator = entry.getTerminator();
    KAI_CHECK(terminator != nullptr);
    if (terminator == nullptr) {
        return;
    }

    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(terminator);
    KAI_CHECK(ret != nullptr);
    if (ret == nullptr) {
        return;
    }

    const llvm::Value* returned = ret->getReturnValue();
    KAI_CHECK(returned != nullptr);
    if (returned == nullptr) {
        return;
    }

    const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(returned);
    KAI_CHECK(constant != nullptr);
    if (constant == nullptr) {
        return;
    }

    KAI_CHECK(constant->getType()->isIntegerTy(64));
    KAI_CHECK(constant->getSExtValue() == 42);

    // Representative generated LLVM IR, for debugging - never asserted
    // against as a snapshot, since exact LLVM textual formatting can
    // differ by version (see this test file's own documentation).
    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    module.print(irStream, nullptr);
    std::cerr << "--- LLVMCodeGeneratorTests: representative IR ---\n" << ir;
}

// TEST 2: two independent zero-argument i64-returning functions.
void testMultipleFunctionsBothExistAndVerify() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn answer() -> i64 {\n    return 42\n}\n"
                                      "fn main() -> i64 {\n    return 7\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();

    llvm::Function* answerFn = module.getFunction("answer");
    llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(answerFn != nullptr);
    KAI_CHECK(mainFn != nullptr);
    if (answerFn == nullptr || mainFn == nullptr) {
        return;
    }

    KAI_CHECK(answerFn->getReturnType()->isIntegerTy(64));
    KAI_CHECK(mainFn->getReturnType()->isIntegerTy(64));

    // generate() already ran llvm::verifyModule() internally and returned
    // true above - re-running it here re-confirms the module is still
    // well-formed after both functions were added, with useful output on
    // failure rather than relying on generate()'s boolean result alone.
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    const bool broken = llvm::verifyModule(module, &errorStream);
    KAI_CHECK(!broken);
    if (broken) {
        std::cerr << "verifyModule reported: " << verifierErrors << '\n';
    }
}

// TEST 3: an unsupported construct (a function parameter) must fail
// generation cleanly rather than emit invalid/partial LLVM IR.
void testUnsupportedParameterFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn identity(x: i64) -> i64 {\n    return x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // The frontend itself accepts this program (a parameterized function
    // returning its own parameter is fully valid KAI) - codegen is the
    // one that must decline it explicitly, since M1 does not lower
    // parameters.
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

} // namespace

int main() {
    testMainReturnsI64Literal();
    testMultipleFunctionsBothExistAndVerify();
    testUnsupportedParameterFailsCleanly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
