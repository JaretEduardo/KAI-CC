// Spellable str + Parameters/Returns MVP (M9): focused codegen coverage for
// `str` as a spellable parameter/return/local type. Kept in its own small
// file (mirroring StringLiteralCodegenTests.cpp's own precedent) rather
// than growing the already-2600+-line LLVMCodeGeneratorTests.cpp.
//
// A key finding this milestone confirmed empirically (not merely by
// inspection): NO production codegen change was needed for parameters,
// returns, calls, forwarding, or recursion - LLVMCodeGenerator's existing
// generic parameter-binding/call/return machinery (declareFunction(),
// defineFunction(), lowerCallExpr(), generateReturnStmt()) already worked
// unmodified for Str once SemanticAnalyzer started producing Type::str()
// signatures for it (see SemanticAnalyzer.cpp's lookupPrimitiveTypeName()).
// These tests exist to PROVE that empirically, especially across function
// boundaries (M9 spec §9), not to exercise new production code.

#include "kai/codegen/LLVMCodeGenerator.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
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

bool verifiesCleanly(const llvm::Module& module) {
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    const bool broken = llvm::verifyModule(module, &errorStream);
    if (broken) {
        std::cerr << "verifyModule reported: " << verifierErrors << '\n';
    }
    return !broken;
}

// A. str parameter LLVM function signature valid: `{ ptr, i64 }` by value,
// not a pointer-to-descriptor / sret shape (M9 spec §8).
void testStrParameterLLVMSignature() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn greet(name: str) {\n    print(name)\n}\nfn main() {\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* greet = codegen.module().getFunction("greet");
    KAI_CHECK(greet != nullptr);
    if (greet == nullptr) {
        return;
    }
    KAI_CHECK(greet->arg_size() == 1);
    KAI_CHECK(greet->getArg(0)->getType()->isStructTy());
    const auto* structTy = llvm::cast<llvm::StructType>(greet->getArg(0)->getType());
    KAI_CHECK(structTy->getNumElements() == 2);
    KAI_CHECK(structTy->getElementType(0)->isPointerTy());
    KAI_CHECK(structTy->getElementType(1)->isIntegerTy(64));
}

// B. str return LLVM signature valid: the function's return type is the
// same `{ ptr, i64 }` struct, by value.
void testStrReturnLLVMSignature() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn language() -> str {\n    return \"KAI\"\n}\nfn main() {\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* language = codegen.module().getFunction("language");
    KAI_CHECK(language != nullptr);
    if (language == nullptr) {
        return;
    }
    KAI_CHECK(language->getReturnType()->isStructTy());
    const auto* structTy = llvm::cast<llvm::StructType>(language->getReturnType());
    KAI_CHECK(structTy->getNumElements() == 2);
    KAI_CHECK(structTy->getElementType(0)->isPointerTy());
    KAI_CHECK(structTy->getElementType(1)->isIntegerTy(64));
}

// C. explicit str local codegens through the existing generic
// alloca/store machinery (no annotation-specific path).
void testExplicitStrLocalCodegens() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let message: str = \"Hello\"\n    print(message)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr || mainFn->empty()) {
        return;
    }
    bool sawStructAlloca = false;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            sawStructAlloca |= alloca->getAllocatedType()->isStructTy();
        }
    }
    KAI_CHECK(sawStructAlloca);
}

// D. user call with a str argument.
void testUserCallWithStrArgument() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn greet(name: str) {\n    print(name)\n}\nfn main() {\n    greet(\"KAI\")\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    const llvm::Function* greetFn = codegen.module().getFunction("greet");
    KAI_CHECK(mainFn != nullptr && greetFn != nullptr);
    if (mainFn == nullptr || greetFn == nullptr) {
        return;
    }
    int callCount = 0;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                callCount += (call->getCalledFunction() == greetFn) ? 1 : 0;
            }
        }
    }
    KAI_CHECK(callCount == 1);
}

// E. user function returning str, called from another function.
void testUserFunctionReturningStr() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn echo(value: str) -> str {\n    return value\n}\nfn main() {\n    print(echo(\"hi\"))\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));
}

// F. forwarding a str parameter through two functions (M9 spec §5).
void testForwardingStrParameter() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn inner(value: str) {\n    print(value)\n}\n"
                                      "fn outer(value: str) {\n    inner(value)\n}\n"
                                      "fn main() {\n    outer(\"forwarded\")\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));
}

// G. a forward call (callee declared textually AFTER the caller)
// returning str - proves the existing two-pass declare-then-define
// architecture needs no Str-specific handling (M9 spec §10).
void testForwardCallReturningStr() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    print(answer())\n}\nfn answer() -> str {\n    return \"KAI\"\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));
}

// H. a recursive function whose signature involves str (M9 spec §11) -
// proves the LLVM Str ABI survives a self-call.
void testRecursiveStrFunction() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn choose_text(value: str, n: i64) -> str {\n"
                                      "    if n == 0 {\n"
                                      "        return value\n"
                                      "    }\n"
                                      "    return choose_text(value, n - 1)\n"
                                      "}\n"
                                      "fn main() {\n    print(choose_text(\"recursed\", 3))\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("choose_text");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    int selfCallCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                selfCallCount += (call->getCalledFunction() == fn) ? 1 : 0;
            }
        }
    }
    KAI_CHECK(selfCallCount == 1);
}

// I./M9 spec §9: type-identity consistency - lowerType(Type::str()) used
// across DIFFERENT functions' signatures within one module must produce
// the SAME (pointer-identical) LLVM struct type, since LLVM only accepts
// a call/return whose argument/result type EXACTLY matches the callee's
// declared type. A literal (unnamed) LLVM struct type is uniqued by
// LLVM per-LLVMContext by its element types - this test proves that
// uniquing actually holds across a parameter, a return type, and an
// alloca, not merely asserts it by reading the source.
void testStrLLVMTypeIdentityAcrossFunctions() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn greet(name: str) {\n    print(name)\n}\n"
                                      "fn language() -> str {\n    return \"KAI\"\n}\n"
                                      "fn main() {\n"
                                      "    let message: str = \"Hello\"\n"
                                      "    greet(message)\n"
                                      "    print(language())\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));

    const llvm::Function* greet = codegen.module().getFunction("greet");
    const llvm::Function* language = codegen.module().getFunction("language");
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(greet != nullptr && language != nullptr && mainFn != nullptr);
    if (greet == nullptr || language == nullptr || mainFn == nullptr || mainFn->empty()) {
        return;
    }

    llvm::Type* paramType = greet->getArg(0)->getType();
    llvm::Type* returnType = language->getReturnType();

    // Pointer equality, not merely structural (isStructTy()/element)
    // equality - this is the exact guarantee lowerCallExpr()'s
    // `(*argument)->getType() != expectedType` check relies on.
    KAI_CHECK(paramType == returnType);

    llvm::Type* localAllocaType = nullptr;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            localAllocaType = alloca->getAllocatedType();
            break;
        }
    }
    KAI_CHECK(localAllocaType != nullptr);
    KAI_CHECK(localAllocaType == paramType);
}

// J. generator-reuse regression (mirrors
// LLVMCodeGeneratorTests.cpp's own testGeneratorReusedAcrossTwoModulesFunctionsDoNotLeak):
// a SECOND generate() call on the same LLVMCodeGenerator instance, using
// Str signatures, must not resolve calls against stale llvm::Function*
// pointers left over from the FIRST (now-replaced) module.
void testGeneratorReuseWithStrSignaturesDoesNotLeak() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);

    Generated first = compileToLLVM(
        sm, codegen, "fn firstOnly() -> str {\n    return \"first\"\n}\nfn main() {\n    print(firstOnly())\n}");
    KAI_CHECK(first.model.errors().empty());
    KAI_CHECK(first.generationSucceeded);

    Generated second = compileToLLVM(
        sm, codegen, "fn secondOnly() -> str {\n    return \"second\"\n}\nfn main() {\n    print(secondOnly())\n}");
    KAI_CHECK(second.model.errors().empty());
    KAI_CHECK(second.generationSucceeded);
    if (!second.generationSucceeded) {
        return;
    }
    KAI_CHECK(verifiesCleanly(codegen.module()));
    // The first module's only-then-existing function must not survive.
    KAI_CHECK(codegen.module().getFunction("firstOnly") == nullptr);
    KAI_CHECK(codegen.module().getFunction("secondOnly") != nullptr);
}

} // namespace

int main() {
    testStrParameterLLVMSignature();
    testStrReturnLLVMSignature();
    testExplicitStrLocalCodegens();
    testUserCallWithStrArgument();
    testUserFunctionReturningStr();
    testForwardingStrParameter();
    testForwardCallReturningStr();
    testRecursiveStrFunction();
    testStrLLVMTypeIdentityAcrossFunctions();
    testGeneratorReuseWithStrSignaturesDoesNotLeak();

    return kai::test::failureCount == 0 ? 0 : 1;
}
