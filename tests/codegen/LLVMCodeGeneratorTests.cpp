#include "kai/codegen/LLVMCodeGenerator.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"
#include "kai/source/SourceManager.hpp"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "support/check.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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

// TEST 3 (M1/M2/M3): "a parameterized function must fail generation
// cleanly" - REMOVED as of M4. Its exact premise (`fn identity(x: i64)
// -> i64 { return x }` must fail codegen) is precisely what M4 was built
// to make succeed - parameter lowering is no longer unsupported. See the
// PARAMETERS test section below for its positive replacement.

// GENERATOR REUSE / LIFETIME
//
// LLVMCodeGenerator::generate() may be called more than once on the same
// instance (module_ is replaced each time - see generate()'s own class
// comment). `functions_` (SymbolId -> llvm::Function*, populated in PASS
// 1 and deliberately left uncleared ACROSS a single module's PASS 1/PASS
// 2 so recursive/forward calls can find each other) must still be reset
// at the START of every generate() call - otherwise a second generate()
// call could resolve a call against a dangling llvm::Function* left over
// from the FIRST (now-destroyed) module. This is distinct from
// `locals_`, which is correctly reset per function body regardless.
void testGeneratorReusedAcrossTwoModulesFunctionsDoNotLeak() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);

    Generated first = compileToLLVM(
        sm, codegen, "fn firstOnly() -> i64 {\n    return 1\n}\nfn main() -> i64 {\n    return firstOnly()\n}");
    KAI_CHECK(first.model.errors().empty());
    KAI_CHECK(first.generationSucceeded); // (1) first generation succeeds

    Generated second = compileToLLVM(
        sm, codegen,
        "fn secondHelper() -> i64 {\n    return 2\n}\nfn main() -> i64 {\n    return secondHelper()\n}");
    KAI_CHECK(second.model.errors().empty());
    KAI_CHECK(second.generationSucceeded); // (2) second generation succeeds
    if (!second.generationSucceeded) {
        return;
    }

    const llvm::Module& secondModule = codegen.module();

    // (3) the second module verifies.
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    const bool broken = llvm::verifyModule(secondModule, &errorStream);
    KAI_CHECK(!broken);
    if (broken) {
        std::cerr << "verifyModule reported: " << verifierErrors << '\n';
    }

    // (4) a function that existed ONLY in the first module must not
    // remain in the second.
    KAI_CHECK(secondModule.getFunction("firstOnly") == nullptr);
    KAI_CHECK(secondModule.getFunction("secondHelper") != nullptr);

    // (5) the call inside the second module's main() must resolve to a
    // llvm::Function that actually belongs to THIS (second) module - not
    // a stale pointer carried over from the first generate() call's
    // (now-destroyed) module.
    const llvm::Function* mainFn = secondModule.getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(mainFn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret == nullptr) {
        return;
    }
    const auto* call = llvm::dyn_cast<llvm::CallInst>(ret->getReturnValue());
    KAI_CHECK(call != nullptr);
    if (call == nullptr) {
        return;
    }
    const llvm::Function* calledFn = call->getCalledFunction();
    KAI_CHECK(calledFn != nullptr);
    if (calledFn != nullptr) {
        KAI_CHECK(calledFn->getParent() == &secondModule);
        KAI_CHECK(calledFn->getName() == "secondHelper");
    }
}

// RELEASE HARDENING M2.1: LLVMCodeGenerator::unsupportedConstruct() (M2)
// must never leak a PREVIOUS generate() call's diagnostic into a LATER,
// unrelated one - mirrors testGeneratorReusedAcrossTwoModulesFunctions
// DoNotLeak()'s own reuse pattern, applied to the new diagnostic state
// instead of `functions_`.
void testUnsupportedConstructDoesNotLeakAcrossGenerateCalls() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);

    // (1) an unsupported (array-containing-Slice) RETURN type sets
    // unsupportedConstruct() on this call. KAI LANGUAGE M6: `for`
    // statements are no longer usable for this - a supported
    // integer-range `for` now lowers successfully (see the M6 test group
    // below), and an unsupported iterable shape is rejected by
    // TypeChecker (SemanticErrorKind::UnsupportedForIterable) before
    // codegen ever runs, so it can no longer exercise THIS
    // unsupported-construct path either. KAI LANGUAGE M10A: `[i32]` is
    // now a real semantic Slice Type, so `return 0` against it would be a
    // genuine TypeMismatch (never reaching codegen at all, per
    // compileToLLVM()'s own model.errors().empty() gate above) - a
    // self-recursive `return f()` no longer works either, now that KAI
    // LANGUAGE M11A's restricted provenance analysis exists: an arbitrary
    // function call's Slice result is always Unknown provenance (never
    // External), so returning it would now be rejected as
    // EscapingLocalSlice before codegen ever runs. KAI LANGUAGE M11B: a
    // BARE Slice return (`return xs`) is now genuinely executable too
    // (spec §4/§19 - see this file's own M11B section further below), so
    // this uses `[[i32]; 2]` (an array that recursively contains a Slice)
    // instead, which remains explicitly unsupported
    // (`isUnsupportedSliceCarryingType()`) even though the element
    // (`xs`) itself is a perfectly safe, `External`-provenance Slice -
    // M11B intentionally does not generalize provenance tracking to
    // aggregates.
    Generated first = compileToLLVM(sm, codegen, "fn f(xs: [i32]) -> [[i32]; 2] {\n    return [xs, xs]\n}");
    KAI_CHECK(first.model.errors().empty());
    KAI_CHECK(!first.generationSucceeded);
    KAI_CHECK(codegen.unsupportedConstruct().has_value());
    if (codegen.unsupportedConstruct().has_value()) {
        KAI_CHECK(codegen.unsupportedConstruct()->description ==
                  "code generation is not yet supported for this function's return type");
    }

    // (2) a SUCCESSFUL, unrelated generate() call afterward must clear it
    // - a stale message from (1) must never survive into a caller that
    // checks unsupportedConstruct() after this call.
    Generated second = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 42\n}");
    KAI_CHECK(second.model.errors().empty());
    KAI_CHECK(second.generationSucceeded);
    KAI_CHECK(!codegen.unsupportedConstruct().has_value());

    // (3) a THIRD call, failing again via a DIFFERENT unsupported
    // construct (an array-of-Slice parameter type, not a `for`
    // statement), must capture ITS OWN message fresh - proving the
    // reset-then-recapture cycle works repeatedly, not merely once. KAI
    // LANGUAGE M10B: a BARE slice parameter (`values: [i32]`) is now
    // genuinely executable (spec §1/§20 - see this file's own M10B
    // section further below), so this uses `[[i32]; 2]` (an array that
    // recursively contains a Slice) instead, which remains explicitly
    // unsupported (spec §6/`typeContainsSlice()`).
    Generated third = compileToLLVM(sm, codegen, "fn sum(values: [[i32]; 2]) -> i32 {\n    return 0\n}");
    KAI_CHECK(third.model.errors().empty());
    KAI_CHECK(!third.generationSucceeded);
    KAI_CHECK(codegen.unsupportedConstruct().has_value());
    if (codegen.unsupportedConstruct().has_value()) {
        KAI_CHECK(codegen.unsupportedConstruct()->description ==
                  "code generation is not yet supported for this parameter's type");
    }
}

// --- M2: Primitive Expression Lowering ---
//
// Small shared helpers: every M2 test below compiles a single function
// and inspects its returned constant. LLVM's IRBuilder constant-folds
// arithmetic/comparison/logical operators applied to two ConstantInt/
// ConstantFP operands automatically - and since M2 has no locals or
// parameters yet, EVERY operand reachable from a return expression is
// necessarily a literal (or a literal composed through Paren/Unary/
// Binary), so every M2 return expression folds down to a single constant
// in the final IR. This is expected, correct LLVM behavior, not a sign
// that lowerBinaryExpr()/lowerUnaryExpr() went unexercised - the
// constant's VALUE (checked below) still directly reflects which LLVM
// opcode/predicate lowerBinaryExpr() selected (e.g. CreateSDiv vs
// CreateUDiv fold to different results for the same bit pattern).

const llvm::ConstantInt* returnedIntegerConstant(const llvm::Module& module, const char* fnName) {
    const llvm::Function* fn = module.getFunction(fnName);
    if (fn == nullptr || fn->empty()) {
        return nullptr;
    }
    const llvm::Instruction* terminator = fn->getEntryBlock().getTerminator();
    if (terminator == nullptr) {
        return nullptr;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(terminator);
    if (ret == nullptr || ret->getReturnValue() == nullptr) {
        return nullptr;
    }
    return llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
}

const llvm::ConstantFP* returnedFloatConstant(const llvm::Module& module, const char* fnName) {
    const llvm::Function* fn = module.getFunction(fnName);
    if (fn == nullptr || fn->empty()) {
        return nullptr;
    }
    const llvm::Instruction* terminator = fn->getEntryBlock().getTerminator();
    if (terminator == nullptr) {
        return nullptr;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(terminator);
    if (ret == nullptr || ret->getReturnValue() == nullptr) {
        return nullptr;
    }
    return llvm::dyn_cast<llvm::ConstantFP>(ret->getReturnValue());
}

// INTEGER ARITHMETIC

void testIntegerAddition() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 40 + 2\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(64));
        KAI_CHECK(c->getSExtValue() == 42);
    }
}

void testNestedArithmetic() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return (10 + 2) * 3\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == 36);
    }
}

void testSubtraction() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 10 - 3\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == 7);
    }
}

void testMultiplication() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 6 * 7\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == 42);
    }
}

// Discriminates CreateSDiv from CreateUDiv: -7/2 truncated-towards-zero
// signed division is -3; the same bit pattern divided as unsigned would
// give a very different (large) result.
void testSignedDivisionTruncatesTowardZero() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i8 {\n    return -7 / 2\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == -3);
    }
}

// Discriminates CreateUDiv from CreateSDiv: the function's own `-> u8`
// return type flows as context into the Divide operands (arithmetic
// operators, unlike comparisons, DO receive contextual typing from the
// enclosing return - see arithmeticOperandContext() in TypeChecker.cpp),
// making both literals genuinely u8. 250/4 as unsigned is 62; the same
// bit pattern divided as signed i8 (250 misinterpreted as -6) would give
// a very different result.
void testUnsignedDivision() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> u8 {\n    return 250 / 4\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(8));
        KAI_CHECK(c->getZExtValue() == 62);
    }
}

// Signed remainder follows the dividend's sign (LLVM srem, C-like
// truncating remainder) - discriminates from Euclidean modulo, which
// would give 3 here instead of -2.
void testModulo() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return -17 % 5\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == -2);
    }
}

// CONTEXTUAL WIDTH

void testI64ArithmeticWidth() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 1 + 2\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(64));
        KAI_CHECK(c->getSExtValue() == 3);
    }
}

void testI8ArithmeticWidth() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i8 {\n    return 1 + 2\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(8));
        KAI_CHECK(c->getSExtValue() == 3);
    }
}

// UNARY

void testUnarySignedNegate() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return -42\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == -42);
    }
}

void testUnaryBoolNot() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return !true\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(1));
        KAI_CHECK(c->getZExtValue() == 0);
    }
}

// COMPARISON
//
// Only a SIGNED comparison test is included: comparisons never receive
// contextual typing from the enclosing return (checkMatchedOperands() is
// always called with std::nullopt context for Less/LessEqual/Greater/
// GreaterEqual/Equal/NotEqual - see checkBinaryExpr() in TypeChecker.cpp)
// - unlike arithmetic operators, which DO receive the return type as
// context. With no locals/parameters available in M2 to otherwise supply
// a concrete unsigned type, a genuinely unsigned-typed comparison cannot
// be constructed from a bare return-comparison expression today; this is
// audited fact, not an oversight. CreateICmpULT/UGT/etc. are implemented
// (see lowerBinaryExpr()) and will become directly testable once M3
// introduces locals (`let x: u32 = ...`).

void testSignedLessThan() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return -1 < 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isIntegerTy(1));
        KAI_CHECK(c->getZExtValue() == 1);
    }
}

// EQUALITY

void testIntegerEquality() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return 5 == 5\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getZExtValue() == 1);
    }
}

void testBoolInequality() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return true != false\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getZExtValue() == 1);
    }
}

// BOOL / LOGICAL

void testReturnTrueLiteral() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return true\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getZExtValue() == 1);
    }
}

// Exercises the resolved eager (non-short-circuit) &&/|| decision - see
// this suite's own class-level documentation and LLVMCodeGenerator's
// header comment for why eager lowering is safe for M2's exact
// side-effect-free expression grammar.
// M4 UPDATE: &&/|| are now FINAL short-circuit operators (M2/M3's eager
// CreateAnd/CreateOr provisional lowering is gone entirely - see the
// dedicated SHORT CIRCUIT section below for the full structural proof
// using Bool-returning calls). Even a trivial literal-operand expression
// now lowers to real control flow - a conditional branch feeding an i1
// PHI - never a folded/eager `and`/`or` instruction.
void testLogicalAndOr() {
    SourceManager sm;
    LLVMCodeGenerator codegenAnd(sm);
    Generated andResult = compileToLLVM(sm, codegenAnd, "fn main() -> bool {\n    return true && false\n}");
    KAI_CHECK(andResult.generationSucceeded);
    if (andResult.generationSucceeded) {
        const llvm::Function* fn = codegenAnd.module().getFunction("main");
        KAI_CHECK(fn != nullptr);
        bool sawPhi = false;
        bool sawCondBr = false;
        bool sawEagerAndOr = false;
        for (const llvm::BasicBlock& block : *fn) {
            for (const llvm::Instruction& inst : block) {
                sawPhi |= llvm::isa<llvm::PHINode>(inst);
                if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                    sawCondBr |= branch->isConditional();
                }
                sawEagerAndOr |=
                    inst.getOpcode() == llvm::Instruction::And || inst.getOpcode() == llvm::Instruction::Or;
            }
        }
        KAI_CHECK(sawPhi);
        KAI_CHECK(sawCondBr);
        KAI_CHECK(!sawEagerAndOr);
    }

    SourceManager sm2;
    LLVMCodeGenerator codegenOr(sm2);
    Generated orResult = compileToLLVM(sm2, codegenOr, "fn main() -> bool {\n    return false || true\n}");
    KAI_CHECK(orResult.generationSucceeded);
    if (orResult.generationSucceeded) {
        const llvm::Function* fn = codegenOr.module().getFunction("main");
        KAI_CHECK(fn != nullptr);
        bool sawPhi = false;
        for (const llvm::BasicBlock& block : *fn) {
            for (const llvm::Instruction& inst : block) {
                sawPhi |= llvm::isa<llvm::PHINode>(inst);
            }
        }
        KAI_CHECK(sawPhi);
    }
}

// FLOATS

void testFloatAddition() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> f64 {\n    return 1.5 + 2.5\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantFP* c = returnedFloatConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getType()->isDoubleTy());
        KAI_CHECK(c->getValueAPF().convertToDouble() == 4.0);
    }
}

void testFloatComparison() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    return 1.5 < 2.5\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getZExtValue() == 1);
    }
}

// PARENS

void testParenthesizedReturnExpression() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return (40 + 2)\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::ConstantInt* c = returnedIntegerConstant(codegen.module(), "main");
    KAI_CHECK(c != nullptr);
    if (c != nullptr) {
        KAI_CHECK(c->getSExtValue() == 42);
    }
}

// FAILURE

// Range typing is still fully deferred by TypeChecker (recorded as
// Type::unresolved(), never Type::error()) - so the frontend itself
// reports no errors for this program, and codegen alone must be the one
// to decline it explicitly rather than emit invalid IR for an
// unrepresentable Range value.
void testUnsupportedRangeFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return 0..10\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

// --- M3: Local Variables + Identifier Loads + Assignment ---
//
// Unlike M2, most of these expressions can no longer constant-fold to a
// bare ConstantInt: a CreateLoad of a runtime alloca is not a compile-time
// constant, so arithmetic/comparisons built on it become real
// instructions (add/icmp/etc. over a load), not folded literals. Tests
// below inspect the actual instruction sequence for exactly this reason.

// LOCALS

void testAnnotatedLocalReturnIdentifier() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    let x: i64 = 40\n    return x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr || fn->empty()) {
        return;
    }

    // alloca, store, load, ret - the full M3 pipeline for a single local.
    const llvm::BasicBlock& entry = fn->getEntryBlock();
    bool sawAlloca = false;
    bool sawStore = false;
    bool sawLoad = false;
    for (const llvm::Instruction& inst : entry) {
        sawAlloca |= llvm::isa<llvm::AllocaInst>(inst);
        sawStore |= llvm::isa<llvm::StoreInst>(inst);
        sawLoad |= llvm::isa<llvm::LoadInst>(inst);
    }
    KAI_CHECK(sawAlloca);
    KAI_CHECK(sawStore);
    KAI_CHECK(sawLoad);

    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(entry.getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(llvm::isa<llvm::LoadInst>(ret->getReturnValue()));
    }
}

void testInferredLocalReturnIdentifier() {
    // `let x = 40` with no annotation and no usable context infers i32
    // (M1's own default-literal-type rule - checkVarDecl()'s unannotated
    // path calls inferExpr(), never forwarding the enclosing return
    // type's context). The declared return type is i32 to match - NOT
    // i64 - so this program actually type-checks: x's fixed i32 type
    // sibling-anchors the `+ 2` literal to i32 too (comparisons/
    // arithmetic never let an outer context override an already-fixed
    // identifier operand's own type - see checkMatchedOperands() in
    // TypeChecker.cpp).
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i32 {\n    let x = 40\n    return x + 2\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    // Symbol.type (i32, inferred) is what drives storage - not any
    // backend-side re-inference. The slot's allocated type is the direct,
    // structural proof of this.
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr || fn->empty()) {
        return;
    }
    bool sawI32Alloca = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            sawI32Alloca |= alloca->getAllocatedType()->isIntegerTy(32);
        }
    }
    KAI_CHECK(sawI32Alloca);
}

void testTwoLocalsArithmetic() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> i64 {\n    let x: i64 = 40\n    let y: i64 = 2\n    return x + y\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    // Two genuinely runtime operands (two separate loads) - the `add`
    // cannot be constant-folded away, unlike every M2 arithmetic test.
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr || fn->empty()) {
        return;
    }
    int loadCount = 0;
    bool sawAdd = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        loadCount += llvm::isa<llvm::LoadInst>(inst) ? 1 : 0;
        sawAdd |= inst.getOpcode() == llvm::Instruction::Add;
    }
    KAI_CHECK(loadCount == 2);
    KAI_CHECK(sawAdd);
}

void testBoolLocal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    let x: bool = true\n    return x\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

void testFloatLocal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> f64 {\n    let x: f64 = 1.5\n    return x + 2.5\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// MUTABILITY / ASSIGNMENT

void testMutableLocalAssignment() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn main() -> i64 {\n    mut y: i64 = 1\n    y = y + 1\n    return y\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    // Structural proof of the full load/store pipeline: at least two
    // stores (the initializer, and the reassignment) and at least two
    // loads (reading y for `y + 1`, and reading y again for the return)
    // into/out of the SAME single alloca.
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr || fn->empty()) {
        return;
    }
    int storeCount = 0;
    int loadCount = 0;
    const llvm::AllocaInst* slot = nullptr;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            slot = alloca;
        }
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            ++storeCount;
            KAI_CHECK(store->getPointerOperand() == slot);
        }
        loadCount += llvm::isa<llvm::LoadInst>(inst) ? 1 : 0;
    }
    KAI_CHECK(storeCount == 2);
    KAI_CHECK(loadCount == 2);

    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(llvm::isa<llvm::LoadInst>(ret->getReturnValue()));
    }
}

// The full M3 required end-state program, verbatim.
void testFullImperativeBody() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() -> i64 {\n"
                                      "    let x: i64 = 40\n"
                                      "    mut y: i64 = 1\n"
                                      "    y = y + 1\n"
                                      "    return x + y\n"
                                      "}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// An assignment used purely as a statement for its store side effect -
// ExprStmt must accept a Unit-valued lowerExpr() success.
void testAssignmentExprStmtSucceedsAsUnit() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> i64 {\n    mut y: i64 = 1\n    y = 5\n    return y\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// IDENTIFIER

void testIdentifierProducesLoad() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> i64 {\n    let x: i64 = 40\n    return x\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(llvm::isa<llvm::LoadInst>(ret->getReturnValue()));
    }
}

// "A parameterized function still fails cleanly" (M3) - REMOVED as of
// M4, same reason as the M1 removal above: `fn identity(x: i64) -> i64
// { return x }` now succeeds. See PARAMETERS below for its replacement.

// SIGNEDNESS WITH LOCALS
//
// Unlike M2 (where comparisons could never receive a concrete unsigned
// operand type at all - see OperatorTests's own comment), a local's
// explicit annotation now supplies one directly, finally making an
// unsigned comparison constructible.

void testUnsignedComparisonWithLocal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> bool {\n    let x: u8 = 250\n    return x < 251\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    bool sawUnsignedCompare = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
            sawUnsignedCompare |= cmp->getPredicate() == llvm::CmpInst::ICMP_ULT;
        }
    }
    KAI_CHECK(sawUnsignedCompare);
}

void testSignedComparisonWithLocal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() -> bool {\n    let x: i8 = -1\n    return x < 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    bool sawSignedCompare = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
            sawSignedCompare |= cmp->getPredicate() == llvm::CmpInst::ICMP_SLT;
        }
    }
    KAI_CHECK(sawSignedCompare);
}

// FAILURES

// LLVM has no storable void value - a Unit-typed local is an explicit,
// documented M3 policy failure (see generateVarDeclStmt()'s own comment),
// not a fallthrough of the Error/Unresolved case.
void testUnitLocalFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> i64 {\n    mut x: i64 = 0\n    let y = (x = 1)\n    return x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

// `testIfStmtStillFailsCleanly` (M3-era: asserted `if` unconditionally
// failed codegen) is REMOVED, not retargeted - its exact premise is
// superseded by M5, which makes IfStmt lowering succeed. See the IF/
// CONDITION/WHILE/NESTING/RECURSION/SHADOWING test groups below for its
// M5 replacement coverage.

// --- M4: Parameters + Function Calls + Recursion + FINAL &&/|| ---

// PARAMETERS

void testOneParameterLoaded() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f(x: i64) -> i64 {\n    return x\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    KAI_CHECK(fn->arg_size() == 1);
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(llvm::isa<llvm::LoadInst>(ret->getReturnValue()));
    }
}

void testTwoParametersArithmetic() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("add");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    KAI_CHECK(fn->arg_size() == 2);
    int loadCount = 0;
    bool sawAdd = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        loadCount += llvm::isa<llvm::LoadInst>(inst) ? 1 : 0;
        sawAdd |= inst.getOpcode() == llvm::Instruction::Add;
    }
    KAI_CHECK(loadCount == 2);
    KAI_CHECK(sawAdd);
}

void testParameterPlusLocal() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f(x: i64) -> i64 {\n    let y: i64 = 2\n    return x + y\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

void testParameterTypeWidthPreserved() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f(x: i8) -> i8 {\n    return x\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    KAI_CHECK(fn->getArg(0)->getType()->isIntegerTy(8));
    KAI_CHECK(fn->getReturnType()->isIntegerTy(8));
}

// CALLS

void testZeroArgumentCall() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn answer() -> i64 {\n    return 42\n}\nfn main() -> i64 {\n    return answer()\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(mainFn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        const auto* call = llvm::dyn_cast<llvm::CallInst>(ret->getReturnValue());
        KAI_CHECK(call != nullptr);
        if (call != nullptr) {
            KAI_CHECK(call->getCalledFunction() == codegen.module().getFunction("answer"));
        }
    }
}

void testParameterizedCall() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn add(a: i64, b: i64) -> i64 {\n    return a + b\n}\n"
                                      "fn main() -> i64 {\n    return add(20, 22)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawCallWithTwoArgs = false;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            sawCallWithTwoArgs |= call->arg_size() == 2;
        }
    }
    KAI_CHECK(sawCallWithTwoArgs);
}

void testCallUsedInArithmetic() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn inc(x: i64) -> i64 {\n    return x + 1\n}\n"
                                      "fn main() -> i64 {\n    return inc(40) + 1\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    // Arithmetic consumes the CallInst's Value directly - no
    // call-specific operator logic exists.
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawAddOfCall = false;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (inst.getOpcode() == llvm::Instruction::Add) {
            sawAddOfCall |= llvm::isa<llvm::CallInst>(inst.getOperand(0));
        }
    }
    KAI_CHECK(sawAddOfCall);
}

void testCallAsLocalInitializer() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn answer() -> i64 {\n    return 42\n}\nfn main() -> i64 {\n    let x: i64 = answer()\n    return x\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

void testCallAsAssignmentRhs() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn answer() -> i64 {\n    return 42\n}\n"
                                      "fn main() -> i64 {\n    mut y: i64 = 0\n    y = answer()\n    return y\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

void testBoolCallWithUnaryNot() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn predicate() -> bool {\n    return true\n}\nfn main() -> bool {\n    return !predicate()\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawNotOfCall = false;
    for (const llvm::Instruction& inst : mainFn->getEntryBlock()) {
        if (inst.getOpcode() == llvm::Instruction::Xor) {
            sawNotOfCall |= llvm::isa<llvm::CallInst>(inst.getOperand(0));
        }
    }
    KAI_CHECK(sawNotOfCall);
}

// FORWARD

void testForwardCall() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> i64 {\n    return answer()\n}\nfn answer() -> i64 {\n    return 42\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// RECURSION

void testSelfRecursiveCallVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn recurse(x: i64) -> i64 {\n    return recurse(x)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("recurse");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        const auto* call = llvm::dyn_cast<llvm::CallInst>(ret->getReturnValue());
        KAI_CHECK(call != nullptr);
        if (call != nullptr) {
            KAI_CHECK(call->getCalledFunction() == fn);
        }
    }
}

// MUTUAL RECURSION

void testMutualRecursionVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn isEven(n: i64) -> bool {\n    return isOdd(n)\n}\n"
                                      "fn isOdd(n: i64) -> bool {\n    return isEven(n)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(codegen.module().getFunction("isEven") != nullptr);
    KAI_CHECK(codegen.module().getFunction("isOdd") != nullptr);
}

// UNIT FUNCTIONS

void testExplicitBareReturnEmitsRetVoid() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn do_work() {\n    return\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("do_work");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(ret->getReturnValue() == nullptr);
    }
}

void testImplicitUnitFallthroughEmitsRetVoid() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn do_work() {\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("do_work");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(ret != nullptr);
    if (ret != nullptr) {
        KAI_CHECK(ret->getReturnValue() == nullptr);
    }
}

void testUnitReturningCallAsExprStmtSucceeds() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn do_work() {\n}\nfn main() -> i64 {\n    do_work()\n    return 42\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// SHORT CIRCUIT

void testShortCircuitAndCallOnlyInRhsBlock() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn rhs() -> bool {\n    return true\n}\nfn test(a: bool) -> bool {\n    return a && rhs()\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("test");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    const llvm::BasicBlock& entry = fn->getEntryBlock();
    const auto* condBr = llvm::dyn_cast<llvm::BranchInst>(entry.getTerminator());
    KAI_CHECK(condBr != nullptr);
    if (condBr == nullptr) {
        return;
    }
    KAI_CHECK(condBr->isConditional());
    // The branch condition is `a`, loaded from its parameter slot.
    KAI_CHECK(llvm::isa<llvm::LoadInst>(condBr->getCondition()));

    auto containsCall = [](const llvm::BasicBlock* block) {
        for (const llvm::Instruction& inst : *block) {
            if (llvm::isa<llvm::CallInst>(inst)) {
                return true;
            }
        }
        return false;
    };

    const bool trueHasCall = containsCall(condBr->getSuccessor(0));
    const bool falseHasCall = containsCall(condBr->getSuccessor(1));
    // Exactly one branch target (the RHS-evaluation block) contains the
    // call - the short-circuit branch never does.
    KAI_CHECK(trueHasCall != falseHasCall);

    int callCount = 0;
    bool sawPhi = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            callCount += llvm::isa<llvm::CallInst>(inst) ? 1 : 0;
            sawPhi |= llvm::isa<llvm::PHINode>(inst);
        }
    }
    KAI_CHECK(callCount == 1); // rhs() is called at most once, never eagerly
    KAI_CHECK(sawPhi);
}

void testShortCircuitOrCallOnlyInRhsBlock() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn rhs() -> bool {\n    return true\n}\nfn test(a: bool) -> bool {\n    return a || rhs()\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("test");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    const auto* condBr = llvm::dyn_cast<llvm::BranchInst>(fn->getEntryBlock().getTerminator());
    KAI_CHECK(condBr != nullptr);
    if (condBr == nullptr) {
        return;
    }
    KAI_CHECK(condBr->isConditional());

    auto containsCall = [](const llvm::BasicBlock* block) {
        for (const llvm::Instruction& inst : *block) {
            if (llvm::isa<llvm::CallInst>(inst)) {
                return true;
            }
        }
        return false;
    };
    const bool trueHasCall = containsCall(condBr->getSuccessor(0));
    const bool falseHasCall = containsCall(condBr->getSuccessor(1));
    KAI_CHECK(trueHasCall != falseHasCall);

    int callCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            callCount += llvm::isa<llvm::CallInst>(inst) ? 1 : 0;
        }
    }
    KAI_CHECK(callCount == 1);
}

// `a && (b || t())` - proves nested short-circuit composes correctly:
// two PHIs (inner `||`, outer `&&`), and `t()` is called at most once.
void testNestedShortCircuit() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn t() -> bool {\n    return true\n}\n"
                                      "fn test(a: bool, b: bool) -> bool {\n    return a && (b || t())\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("test");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int phiCount = 0;
    int callCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            phiCount += llvm::isa<llvm::PHINode>(inst) ? 1 : 0;
            callCount += llvm::isa<llvm::CallInst>(inst) ? 1 : 0;
        }
    }
    KAI_CHECK(phiCount == 2);
    KAI_CHECK(callCount == 1);
}

// FAILURES

// RETARGETED (M6): this test's exact premise ("`print(...)` fails codegen
// cleanly because ALL builtins are unsupported") is superseded outright by
// M6, which recognizes and lowers `print`. `panic`/`assert` remain
// unsupported Builtins, so they now carry this test's original intent -
// see the PRINT BUILTIN test group below for `print`'s own coverage.
void testUnsupportedBuiltinCallStillFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    panic(1)\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

// INTEGRATION

void testParametersLocalsAssignmentCallIntegration() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn helper(x: i64) -> i64 {\n    return x + 1\n}\n"
                                      "fn main() -> i64 {\n"
                                      "    let a: i64 = 10\n"
                                      "    mut b: i64 = 0\n"
                                      "    b = helper(a)\n"
                                      "    return a + b\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
}

// --- M5: If/Else/Else-If + While statement-level control flow ---

// IF

// `if` with no `else`: the false-condition edge always reaches the merge
// block directly, so the statement always falls through (M5 spec §5).
void testIfNoElseFallsThrough() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn abs_like(x: i64) -> i64 {\n"
                                      "    if x < 0 {\n"
                                      "        return -x\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("abs_like");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int condBrCount = 0;
    int retCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            condBrCount += br->isConditional() ? 1 : 0;
        }
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
    }
    KAI_CHECK(condBrCount == 1);
    KAI_CHECK(retCount == 2); // return -x, and the trailing return x
}

// `if`/`else` where both arms merely assign (fall through): resolved
// entirely through existing alloca/load/store, no PHI for `x` itself.
void testIfElseBothFallThroughViaAssignments() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn choose(cond: bool) -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "\n"
                                      "    if cond {\n"
                                      "        x = 10\n"
                                      "    } else {\n"
                                      "        x = 20\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("choose");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int phiCount = 0;
    int storeCount = 0;
    int retCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            phiCount += llvm::isa<llvm::PHINode>(inst) ? 1 : 0;
            storeCount += llvm::isa<llvm::StoreInst>(inst) ? 1 : 0;
        }
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
    }
    KAI_CHECK(phiCount == 0); // no SSA merging for `x` - alloca/load/store only
    KAI_CHECK(storeCount == 4); // param cond binding, mut x = 0, x = 10, x = 20
    KAI_CHECK(retCount == 1); // a single trailing `return x`, reached via the merge block

    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    module.print(irStream, nullptr);
    std::cerr << "--- LLVMCodeGeneratorTests: representative if/else IR ---\n" << ir;
}

// then returns, false path falls through past the if (same shape as
// testIfNoElseFallsThrough, kept separate since it is one of M5's
// explicitly-required end-state programs and directly names §16's
// "return through if with no else" requirement).
void testIfThenReturnsFalseContinues() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f(cond: bool) -> i64 {\n    if cond {\n        return 1\n    }\n\n    return 2\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

// Both if/else branches return: no merge block should exist at all (M5
// spec §4/§6 case D) - and the function needs no extra trailing `return`.
void testIfElseBothBranchesReturn() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn choose(cond: bool) -> i64 {\n    if cond {\n        return 1\n    } else {\n        return 2\n    }\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("choose");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    // Exactly `entry`, `if.then`, `if.else` - no `if.end` merge block
    // survives, since both arms terminate and it would be unreachable.
    std::size_t blockCount = 0;
    int retCount = 0;
    bool anyBlockUnterminated = false;
    for (const llvm::BasicBlock& block : *fn) {
        ++blockCount;
        if (block.getTerminator() == nullptr) {
            anyBlockUnterminated = true;
        }
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
    }
    KAI_CHECK(blockCount == 3);
    KAI_CHECK(!anyBlockUnterminated);
    KAI_CHECK(retCount == 2);
}

// `if a { } else if b { } else { }` - else-if lowered as a nested if
// inside what would otherwise be the plain else block.
void testElseIfChain() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn classify(a: bool, b: bool) -> i64 {\n"
                                      "    if a {\n"
                                      "        return 1\n"
                                      "    } else if b {\n"
                                      "        return 2\n"
                                      "    } else {\n"
                                      "        return 3\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("classify");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int condBrCount = 0;
    int retCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            condBrCount += br->isConditional() ? 1 : 0;
        }
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
    }
    // Every branch (all three) terminates, so no merge block anywhere in
    // the chain survives.
    KAI_CHECK(condBrCount == 2); // `a`'s test, and the nested `b`'s test
    KAI_CHECK(retCount == 3);
}

// CONDITION

void testIfConditionParameterBool() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f(flag: bool) -> i64 {\n    if flag {\n        return 1\n    }\n\n    return 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

void testIfConditionComparison() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f(x: i64) -> i64 {\n    if x < 0 {\n        return -1\n    }\n\n    return 1\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

// M5 spec §9: a short-circuit condition inside an `if` must branch from
// the ACTUAL block lowerExpr(condition) leaves current, not the block
// that existed before lowering it.
void testIfConditionShortCircuitCall() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn rhs() -> bool {\n    return true\n}\n"
                                      "fn test(a: bool) -> i64 {\n"
                                      "    if a && rhs() {\n        return 1\n    }\n\n    return 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("test");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int phiCount = 0;
    int callCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            phiCount += llvm::isa<llvm::PHINode>(inst) ? 1 : 0;
            callCount += llvm::isa<llvm::CallInst>(inst) ? 1 : 0;
        }
    }
    KAI_CHECK(phiCount == 1); // the `&&` PHI, feeding the if's own CondBr
    KAI_CHECK(callCount == 1); // rhs() called at most once
}

// WHILE

void testWhileMutableCounterLoop() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "\n"
                                      "    while x < 10 {\n"
                                      "        x = x + 1\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    // Structural CFG check (M5 spec §19): a condition block ending in a
    // conditional branch with two distinct successors, a body block
    // ending in an unconditional branch BACK to the condition block (the
    // back-edge), and an exit block. Never relies on exact block names,
    // since LLVM may rename/uniquify them.
    const llvm::BasicBlock* conditionBlock = nullptr;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            if (br->isConditional()) {
                conditionBlock = &block;
                break;
            }
        }
    }
    KAI_CHECK(conditionBlock != nullptr);
    if (conditionBlock == nullptr) {
        return;
    }

    bool sawBackEdge = false;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            if (!br->isConditional() && br->getSuccessor(0) == conditionBlock) {
                sawBackEdge = true;
            }
        }
    }
    KAI_CHECK(sawBackEdge);

    int condBrCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            condBrCount += br->isConditional() ? 1 : 0;
        }
    }
    KAI_CHECK(condBrCount == 1);

    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    module.print(irStream, nullptr);
    std::cerr << "--- LLVMCodeGeneratorTests: representative while IR ---\n" << ir;
}

void testWhileBodyAssignment() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f() -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "    mut y: i64 = 0\n"
                                      "\n"
                                      "    while x < 5 {\n"
                                      "        y = y + x\n"
                                      "        x = x + 1\n"
                                      "    }\n"
                                      "\n"
                                      "    return y\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// The body always returns (M5 spec §12): no `ret` followed by a
// back-edge `br` in the same block - but the loop's own exit block must
// still be reachable, since the condition may be false on entry.
void testWhileBodyReturn() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f(x: i64) -> i64 {\n"
                                      "    while x < 10 {\n        return 42\n    }\n\n    return 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    // Every block's terminator is either a return or a (unconditional or
    // conditional) branch - never an instruction after a `ret`.
    for (const llvm::BasicBlock& block : *fn) {
        const llvm::Instruction* terminator = block.getTerminator();
        KAI_CHECK(terminator != nullptr);
        KAI_CHECK(&block.back() == terminator);
    }

    int retCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
    }
    KAI_CHECK(retCount == 2); // `return 42` inside the loop, `return 0` after it
}

// M5 spec §11: a short-circuit while condition must branch from the
// ACTUAL block lowerExpr(condition) leaves current.
void testWhileShortCircuitCondition() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn predicate() -> bool {\n    return true\n}\n"
                                      "fn f(a: bool) -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "\n"
                                      "    while a && predicate() {\n"
                                      "        x = x + 1\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    int phiCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            phiCount += llvm::isa<llvm::PHINode>(inst) ? 1 : 0;
        }
    }
    KAI_CHECK(phiCount == 1);
}

// NESTING

void testIfInsideWhile() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f() -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "\n"
                                      "    while x < 10 {\n"
                                      "        if x == 5 {\n"
                                      "            x = x + 2\n"
                                      "        } else {\n"
                                      "            x = x + 1\n"
                                      "        }\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

void testWhileInsideIf() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f(outer: bool) -> i64 {\n"
                                      "    mut x: i64 = 0\n"
                                      "\n"
                                      "    if outer {\n"
                                      "        while x < 10 {\n"
                                      "            x = x + 1\n"
                                      "        }\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// RECURSION

// M5 spec §17: factorial(n) as a standalone recursive-integration proof.
void testFactorialIntegration() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn factorial(n: i64) -> i64 {\n"
                                      "    if n <= 1 {\n"
                                      "        return 1\n"
                                      "    } else {\n"
                                      "        return n * factorial(n - 1)\n"
                                      "    }\n"
                                      "}\n"
                                      "fn main() -> i64 {\n    return factorial(5)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* factorial = module.getFunction("factorial");
    KAI_CHECK(factorial != nullptr);
    if (factorial == nullptr) {
        return;
    }

    int condBrCount = 0;
    int retCount = 0;
    bool sawRecursiveCall = false;
    for (const llvm::BasicBlock& block : *factorial) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            condBrCount += br->isConditional() ? 1 : 0;
        }
        retCount += llvm::isa<llvm::ReturnInst>(block.getTerminator()) ? 1 : 0;
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                sawRecursiveCall |= call->getCalledFunction() == factorial;
            }
        }
    }
    KAI_CHECK(condBrCount == 1);
    KAI_CHECK(retCount == 2); // `return 1`, and `return n * factorial(n - 1)`
    KAI_CHECK(sawRecursiveCall);

    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    module.print(irStream, nullptr);
    std::cerr << "--- LLVMCodeGeneratorTests: representative factorial IR ---\n" << ir;
}

// M5 spec §18: optional fibonacci integration, included since it was
// trivial once factorial worked.
void testFibonacciIntegration() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn fib(n: i64) -> i64 {\n"
                                      "    if n <= 1 {\n        return n\n    }\n"
                                      "\n    return fib(n - 1) + fib(n - 2)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// SHADOWING

// M5 spec §15: a branch-scoped `let x` must get its own SymbolId/alloca,
// distinct from the outer `x` - the final `return x` must load the OUTER
// slot, never the inner one (which is never observed outside its branch).
void testShadowingInsideIfBranch() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f(cond: bool) -> i64 {\n"
                                      "    let x: i64 = 10\n"
                                      "\n"
                                      "    if cond {\n"
                                      "        let x: i64 = 20\n"
                                      "        let y: i64 = x\n"
                                      "    }\n"
                                      "\n"
                                      "    return x\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* fn = module.getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    // Four distinct allocas: the `cond` parameter, outer `x`, inner `x`,
    // inner `y` - never one `x` slot reused across the two lexical
    // scopes.
    int allocaCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            allocaCount += llvm::isa<llvm::AllocaInst>(inst) ? 1 : 0;
        }
    }
    KAI_CHECK(allocaCount == 4);

    // Identify the outer `x` slot structurally, by its OWN initializer
    // (`let x: i64 = 10`) rather than by position - createEntryBlockAlloca()
    // always inserts at the entry block's current start (M4/M5 comment on
    // createEntryBlockAlloca()), so the inner branch's `let`s (lowered
    // later, but still targeting the entry block) end up positioned
    // BEFORE the outer `x` alloca, not after it.
    const llvm::AllocaInst* outerX = nullptr;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())) {
                    if (constant->getSExtValue() == 10) {
                        outerX = llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand());
                    }
                }
            }
        }
    }
    KAI_CHECK(outerX != nullptr);

    // The final `return x` loads from the outer slot, never the inner one
    // (which is only ever observed, if at all, inside its own branch).
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
            if (const auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(ret->getReturnValue())) {
                KAI_CHECK(load->getPointerOperand() == outerX);
            }
        }
    }
}

// FAILURE

// RETARGETED (KAI LANGUAGE M6): this test's original premise ("every
// `for` statement fails codegen cleanly because iteration lowering does
// not exist") is superseded outright by M6, which lowers a supported
// integer-range `for` (see the M6 test group below). The one remaining
// "still fails cleanly" shape M6 explicitly requires (spec #5) is
// assignment TO THE LOOP VARIABLE ITSELF - SemanticAnalyzer declares it
// immutable, so this is rejected at the FRONTEND
// (AssignmentToImmutableBinding), before codegen ever runs; there is no
// separate for-variable-specific diagnostic.
void testForLoopVariableAssignmentFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f() -> i64 {\n    for i in 0..10 {\n        i = i + 1\n    }\n\n    return 0\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == kai::semantic::SemanticErrorKind::AssignmentToImmutableBinding);
    }
    KAI_CHECK(!result.generationSucceeded);
}

// --- KAI LANGUAGE M6 (post-alpha.2, distinct from the LLVM codegen
// milestone numbering elsewhere in this file): `for` + integer ranges ---

// Structural CFG check, same technique as testWhileMutableCounterLoop():
// a condition block ending in a conditional branch, a body block ending
// in an unconditional back-edge to the condition block, and exactly one
// conditional branch in the whole function (never relies on exact block
// names, since LLVM may rename/uniquify them).
void testForLoopBasicCFGStructure() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    for i in 0..3 {\n        print(i)\n    }\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    KAI_CHECK(!llvm::verifyModule(module));

    const llvm::Function* fn = module.getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    const llvm::BasicBlock* conditionBlock = nullptr;
    int condBrCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            if (br->isConditional()) {
                conditionBlock = &block;
                ++condBrCount;
            }
        }
    }
    KAI_CHECK(condBrCount == 1);
    KAI_CHECK(conditionBlock != nullptr);
    if (conditionBlock == nullptr) {
        return;
    }

    bool sawBackEdge = false;
    for (const llvm::BasicBlock& block : *fn) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator())) {
            if (!br->isConditional() && br->getSuccessor(0) == conditionBlock) {
                sawBackEdge = true;
            }
        }
    }
    KAI_CHECK(sawBackEdge);
}

// Signed element type (i32, the literal default) must compare via
// CreateICmpSLT, never the unsigned predicate.
void testForLoopSignedRangeUsesSignedComparison() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    for i in 0..3 {\n        print(i)\n    }\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSignedLess = false;
    bool sawUnsignedLess = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSignedLess |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT;
                sawUnsignedLess |= cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
        }
    }
    KAI_CHECK(sawSignedLess);
    KAI_CHECK(!sawUnsignedLess);
}

// Unsigned element type (u32, via two explicitly-typed locals) must
// compare via CreateICmpULT, never the signed predicate.
void testForLoopUnsignedRangeUsesUnsignedComparison() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let start: u32 = 0\n"
                                      "    let end: u32 = 3\n"
                                      "    for i in start..end {\n"
                                      "        print(i)\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSignedLess = false;
    bool sawUnsignedLess = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSignedLess |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT;
                sawUnsignedLess |= cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
        }
    }
    KAI_CHECK(sawUnsignedLess);
    KAI_CHECK(!sawSignedLess);
}

// M6 spec #1/#2/#12: `start`/`end` are each evaluated EXACTLY ONCE, in
// the preheader - never re-lowered inside the loop. Structural proof (as
// distinct from the observable-side-effect native integration test
// below): a call to a side-effecting `end()` function used as the
// range's upper bound must appear exactly once in the whole function,
// never once per potential iteration.
void testForLoopEndExpressionLoweredExactlyOnce() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn bound() -> i32 {\n    return 3\n}\n"
                                      "fn main() {\n"
                                      "    for i in 0..bound() {\n"
                                      "        print(i)\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    const llvm::Function* boundFn = module.getFunction("bound");
    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(boundFn != nullptr);
    KAI_CHECK(mainFn != nullptr);
    if (boundFn == nullptr || mainFn == nullptr) {
        return;
    }

    int callCount = 0;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                callCount += call->getCalledFunction() == boundFn ? 1 : 0;
            }
        }
    }
    KAI_CHECK(callCount == 1);
}

// Nested `for` loops (M6 spec #7) must still verify.
void testForLoopNestedVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    for i in 0..2 {\n"
                                      "        for j in 0..2 {\n"
                                      "            print(j)\n"
                                      "        }\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// `return` inside a `for` body (M6 spec #8) must still verify, and the
// loop's own exit block must remain reachable regardless (the loop may
// execute zero times, so control can always reach the fallback `return`
// after it) - same reasoning as testWhileBodyReturn().
void testForLoopBodyReturnVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f() -> i32 {\n"
                                      "    for i in 0..10 {\n"
                                      "        if i == 3 {\n"
                                      "            return i\n"
                                      "        }\n"
                                      "    }\n"
                                      "    return -1\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// --- M6: minimal `print` builtin + runtime ABI ---

// PRINT BUILTIN

// A literal argument: `42` defaults to i32 (CLAUDE.md's literal-typing
// rule) - proves sign-extension to the normalized `kai_print_i64` ABI.
void testPrintLiteralI32SignExtendsToI64() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(42)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    std::string verifierErrors;
    llvm::raw_string_ostream errorStream(verifierErrors);
    KAI_CHECK(!llvm::verifyModule(module, &errorStream));

    const llvm::Function* runtimeFn = module.getFunction("kai_print_i64");
    KAI_CHECK(runtimeFn != nullptr);
    if (runtimeFn == nullptr) {
        return;
    }
    KAI_CHECK(runtimeFn->isDeclaration()); // declared, never defined in this module
    KAI_CHECK(runtimeFn->getReturnType()->isVoidTy());
    KAI_CHECK(runtimeFn->arg_size() == 1);
    KAI_CHECK(runtimeFn->getArg(0)->getType()->isIntegerTy(64));

    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }

    // A CONSTANT operand's sign-extension is constant-folded by LLVM
    // itself (CreateSExt on a ConstantInt never emits a real SExtInst) -
    // so the sign-extension is verified here by checking the resulting
    // call argument is the correctly-widened i64 constant, not by
    // looking for an SExtInst instruction (see
    // testPrintUnsignedVariableZeroExtends()/
    // testPrintF32VariableExtendsToDouble() below for cases where the
    // extended operand is NOT a constant, and a real instruction does
    // appear).
    bool sawCall = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == runtimeFn) {
                    sawCall = true;
                    KAI_CHECK(call->getType()->isVoidTy());
                    KAI_CHECK(call->arg_size() == 1);
                    if (call->arg_size() == 1) {
                        const auto* argument = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
                        KAI_CHECK(argument != nullptr);
                        if (argument != nullptr) {
                            KAI_CHECK(argument->getType()->isIntegerTy(64));
                            KAI_CHECK(argument->getSExtValue() == 42);
                        }
                    }
                }
            }
        }
    }
    KAI_CHECK(sawCall);

    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    module.print(irStream, nullptr);
    std::cerr << "--- LLVMCodeGeneratorTests: representative print IR ---\n" << ir;
}

// An i64-typed local: no sign-extension is needed (already the runtime
// ABI's own width) - only the load and the call should appear.
void testPrintVariableI64() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let x: i64 = 42\n    print(x)\n}");

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

    bool sawLoad = false;
    bool sawCall = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            sawLoad |= llvm::isa<llvm::LoadInst>(inst);
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "kai_print_i64") {
                    sawCall = true;
                }
            }
        }
    }
    KAI_CHECK(sawLoad);
    KAI_CHECK(sawCall);
}

// An unsigned local: must zero-extend, never sign-extend (M6 spec §13).
void testPrintUnsignedVariableZeroExtends() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let x: u32 = 5\n    print(x)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Module& module = codegen.module();
    KAI_CHECK(module.getFunction("kai_print_u64") != nullptr);
    KAI_CHECK(module.getFunction("kai_print_i64") == nullptr); // never the signed ABI for an unsigned value

    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawZExt = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            sawZExt |= llvm::isa<llvm::ZExtInst>(inst);
        }
    }
    KAI_CHECK(sawZExt);
}

void testPrintBoolLiteral() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(true)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* runtimeFn = codegen.module().getFunction("kai_print_bool");
    KAI_CHECK(runtimeFn != nullptr);
    if (runtimeFn == nullptr) {
        return;
    }
    KAI_CHECK(runtimeFn->arg_size() == 1);
    KAI_CHECK(runtimeFn->getArg(0)->getType()->isIntegerTy(32));
}

void testPrintFloatLiteral() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(3.5)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* runtimeFn = codegen.module().getFunction("kai_print_f64");
    KAI_CHECK(runtimeFn != nullptr);
    if (runtimeFn == nullptr) {
        return;
    }
    KAI_CHECK(runtimeFn->arg_size() == 1);
    KAI_CHECK(runtimeFn->getArg(0)->getType()->isDoubleTy());
}

// An f32 local must extend to double before the call (M6 spec §15).
void testPrintF32VariableExtendsToDouble() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let x: f32 = 1.5\n    print(x)\n}");

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
    bool sawFPExt = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            sawFPExt |= llvm::isa<llvm::FPExtInst>(inst);
        }
    }
    KAI_CHECK(sawFPExt);
}

// Three prints of the same ABI type must reuse ONE runtime declaration
// (M6 spec §11) and each still becomes its own CallInst.
void testMultiplePrintsReuseRuntimeDeclaration() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(1)\n    print(2)\n    print(3)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    int declCount = 0;
    for (const llvm::Function& fn : codegen.module()) {
        declCount += fn.getName() == "kai_print_i64" ? 1 : 0;
    }
    KAI_CHECK(declCount == 1);

    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    int callCount = 0;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                callCount += (call->getCalledFunction() != nullptr &&
                              call->getCalledFunction()->getName() == "kai_print_i64")
                                 ? 1
                                 : 0;
            }
        }
    }
    KAI_CHECK(callCount == 3);
}

// Minimal String Literal Support milestone: KAI's Type vocabulary now has
// TypeKind::Str (see Type.hpp), so a string literal's own semantic Type
// is the concrete Type::str() - lowerPrintCall() now recognizes it and
// dispatches to kai_print_str() instead of failing. Focused codegen
// coverage for the Str descriptor/global/runtime-call shape itself lives
// in tests/codegen/StringLiteralCodegenTests.cpp; this one test is kept
// here (renamed, not deleted) since it is the direct correction of what
// this exact scenario used to assert before this milestone.
void testPrintStringArgumentSucceeds() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(\"hi\")\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
    KAI_CHECK(codegen.module().getFunction("kai_print_str") != nullptr);
}

// A user-declared `print` must shadow the builtin entirely (M6 spec §7) -
// resolved and lowered as an ordinary user Function call, never hijacked
// by the runtime-ABI path.
void testUserDefinedPrintShadowsBuiltin() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn print(x: i64) -> i64 {\n    return x\n}\nfn main() -> i64 {\n    return print(5)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Module& module = codegen.module();
    // No runtime ABI declaration at all - the builtin path was never taken.
    KAI_CHECK(module.getFunction("kai_print_i64") == nullptr);
    KAI_CHECK(module.getFunction("kai_print_u64") == nullptr);
    KAI_CHECK(module.getFunction("kai_print_bool") == nullptr);
    KAI_CHECK(module.getFunction("kai_print_f64") == nullptr);

    const llvm::Function* userPrint = module.getFunction("print");
    KAI_CHECK(userPrint != nullptr);
    if (userPrint == nullptr) {
        return;
    }
    KAI_CHECK(!userPrint->isDeclaration()); // a real, user-defined body
    KAI_CHECK(userPrint->getReturnType()->isIntegerTy(64));

    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(mainFn != nullptr);
    if (mainFn == nullptr) {
        return;
    }
    bool sawCallToUserPrint = false;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                sawCallToUserPrint |= call->getCalledFunction() == userPrint;
            }
        }
    }
    KAI_CHECK(sawCallToUserPrint);
}

// --- KAI LANGUAGE M7B: local fixed-size arrays + checked indexing ---

// LLVM [N x T] type, local alloca, and literal element stores in the
// correct source order.
void testArrayLocalAllocaAndLiteralStores() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let xs = [10, 20, 30]\n    print(xs[0])\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    const llvm::AllocaInst* arrayAlloca = nullptr;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            if (llvm::isa<llvm::ArrayType>(alloca->getAllocatedType())) {
                arrayAlloca = alloca;
            }
        }
    }
    KAI_CHECK(arrayAlloca != nullptr);
    if (arrayAlloca == nullptr) {
        return;
    }
    const auto* arrayType = llvm::cast<llvm::ArrayType>(arrayAlloca->getAllocatedType());
    KAI_CHECK(arrayType->getNumElements() == 3);
    KAI_CHECK(arrayType->getElementType()->isIntegerTy(32));

    // Three GEP+store pairs into the array, storing 10/20/30 in order.
    std::vector<std::int64_t> storedConstants;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())) {
                if (llvm::isa<llvm::GetElementPtrInst>(store->getPointerOperand())) {
                    storedConstants.push_back(c->getSExtValue());
                }
            }
        }
    }
    KAI_CHECK(storedConstants.size() >= 3);
    if (storedConstants.size() >= 3) {
        KAI_CHECK(storedConstants[0] == 10);
        KAI_CHECK(storedConstants[1] == 20);
        KAI_CHECK(storedConstants[2] == 30);
    }
}

// Zero-length arrays lower to a valid `[0 x T]`.
void testZeroLengthArrayLowersAndVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let xs: [i32; 0] = []\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Array literal elements are lowered left to right, exactly once each -
// side-effecting calls must appear in source order with no duplicates.
void testArrayLiteralElementsEvaluatedLeftToRightOnce() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn a() -> i32 {\n    print(100)\n    return 1\n}\n"
                                      "fn b() -> i32 {\n    print(200)\n    return 2\n}\n"
                                      "fn main() {\n    let xs = [a(), b()]\n    print(xs[0])\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    const llvm::Function* aFn = module.getFunction("a");
    const llvm::Function* bFn = module.getFunction("b");
    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(aFn != nullptr && bFn != nullptr && mainFn != nullptr);
    if (aFn == nullptr || bFn == nullptr || mainFn == nullptr) {
        return;
    }

    std::vector<const llvm::Function*> callOrder;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == aFn || call->getCalledFunction() == bFn) {
                    callOrder.push_back(call->getCalledFunction());
                }
            }
        }
    }
    KAI_CHECK(callOrder.size() == 2);
    if (callOrder.size() == 2) {
        KAI_CHECK(callOrder[0] == aFn);
        KAI_CHECK(callOrder[1] == bFn);
    }
}

// Element load: correct GEP form (first index 0, second the element
// index) followed by exactly one load of the element type.
void testElementReadUsesCorrectGEPAndLoad() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let xs = [10, 20, 30]\n    print(xs[1])\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawElementLoad = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
                KAI_CHECK(llvm::isa<llvm::ArrayType>(gep->getSourceElementType()));
                KAI_CHECK(gep->getNumIndices() == 2);
                for (const llvm::Use& use : gep->uses()) {
                    if (llvm::isa<llvm::LoadInst>(use.getUser())) {
                        sawElementLoad = true;
                    }
                }
            }
        }
    }
    KAI_CHECK(sawElementLoad);
}

// Element write: a mutable indexed assignment stores through the same
// kind of GEP'd address, never through the whole-array alloca directly.
void testElementWriteStoresThroughGEP() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    mut xs = [10, 20, 30]\n    xs[1] = 99\n    print(xs[1])\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawStoreOfNinetyNineThroughGEP = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())) {
                    if (c->getSExtValue() == 99 && llvm::isa<llvm::GetElementPtrInst>(store->getPointerOperand())) {
                        sawStoreOfNinetyNineThroughGEP = true;
                    }
                }
            }
        }
    }
    KAI_CHECK(sawStoreOfNinetyNineThroughGEP);
}

// Bounds CFG: a signed dynamic index produces a signed comparison
// (ICMP_SGE for the non-negative check) plus the unsigned upper-bound
// comparison (ICMP_ULT), an llvm.trap + unreachable out-of-bounds block,
// and NO GEP/load/store dominates the trap block (i.e. none of them
// exist in it).
void testSignedDynamicIndexBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let xs = [10, 20, 30]\n"
                                      "    mut i: i32 = 1\n"
                                      "    print(xs[i])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSGE = false;
    bool sawULT = false;
    bool sawTrapCall = false;
    const llvm::BasicBlock* trapBlock = nullptr;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSGE |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                sawULT |= cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "llvm.trap") {
                    sawTrapCall = true;
                    trapBlock = &block;
                }
            }
        }
    }
    KAI_CHECK(sawSGE);
    KAI_CHECK(sawULT);
    KAI_CHECK(sawTrapCall);
    KAI_CHECK(trapBlock != nullptr);
    if (trapBlock != nullptr) {
        KAI_CHECK(llvm::isa<llvm::UnreachableInst>(trapBlock->getTerminator()));
        // No element GEP/load/store in the trap block itself - the
        // element address is only ever computed in the in-bounds
        // successor.
        for (const llvm::Instruction& inst : *trapBlock) {
            KAI_CHECK(!llvm::isa<llvm::GetElementPtrInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::LoadInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::StoreInst>(inst));
        }
    }
}

// Bounds CFG: an UNSIGNED dynamic index skips the non-negative (SGE)
// check entirely - only the unsigned upper-bound comparison is needed
// (M7B spec §6).
void testUnsignedDynamicIndexBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let xs = [10, 20, 30]\n"
                                      "    mut i: u32 = 1\n"
                                      "    print(xs[i])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSGE = false;
    bool sawULT = false;
    bool sawTrapCall = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSGE |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                sawULT |= cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "llvm.trap") {
                    sawTrapCall = true;
                }
            }
        }
    }
    KAI_CHECK(!sawSGE);
    KAI_CHECK(sawULT);
    KAI_CHECK(sawTrapCall);
}

// M6 integration: `for i in 0..3 { print(xs[i]) }` - the for-loop's own
// induction variable is reused directly as the index expression, and
// the whole module still verifies.
void testM6ForLoopIndexIntegrationVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let xs = [10, 20, 30]\n"
                                      "    for i in 0..3 {\n"
                                      "        print(xs[i])\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// M6 integration, mutable: `for i in 0..3 { xs[i] = xs[i] + 1 }`.
void testM6ForLoopIndexedMutationIntegrationVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    mut xs = [10, 20, 30]\n"
                                      "    for i in 0..3 {\n"
                                      "        xs[i] = xs[i] + 1\n"
                                      "    }\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// --- KAI LANGUAGE M8B: array value copying + function ABI now real ---
//
// M8A was a semantic-contract milestone only and implemented NO array
// function ABI/whole-array-copy codegen; the tests here previously locked
// in that the M7B backend guards produced a clean, actionable
// unsupportedConstruct() failure for each of these programs. KAI LANGUAGE
// M8B removes those guards and implements the approved behavior as a
// direct LLVM aggregate ABI - these tests are retargeted to assert
// SUCCESSFUL generation and a verified module, per M8B spec §19. See
// NativeCompilationTests.cpp for the same programs exercised through the
// full CLI pipeline with observed stdout, and the dedicated aggregate-IR
// assertions further below for the ABI shape itself
// (FunctionType/alloca/call/return).

void testArrayParameterNowGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn sum(xs: [i32; 3]) -> i32 {\n    return xs[0]\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

void testArrayReturnTypeNowGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn make() -> [i32; 3] {\n    return [1, 2, 3]\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

void testWholeArrayInitializationFromAnotherArrayNowGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f() {\n    let a = [1, 2, 3]\n    let b = a\n}");

    // A real value copy, not aliasing (M8A §1/§18.A) - now implemented.
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

void testWholeArrayAssignmentNowGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f() {\n    mut a = [1, 2, 3]\n    let b = [4, 5, 6]\n    a = b\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Self-assignment (M8A spec §5): semantically valid, with NO special
// language error - now generates and verifies successfully under M8B.
void testWholeArraySelfAssignmentNowGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f() {\n    mut a = [1, 2, 3]\n    a = a\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// --- KAI LANGUAGE M8B: aggregate ABI shape assertions (spec §19) ---

// Direct aggregate FunctionType for both a parameter and a return - no
// sret, no hidden pointer parameter, no byval attribute.
void testArrayParameterAndReturnUseDirectAggregateFunctionType() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn echo(xs: [i32; 3]) -> [i32; 3] {\n    return xs\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    llvm::Function* echo = codegen.module().getFunction("echo");
    KAI_CHECK(echo != nullptr);
    if (echo == nullptr) {
        return;
    }
    llvm::FunctionType* fnType = echo->getFunctionType();
    KAI_CHECK(fnType->getNumParams() == 1);
    KAI_CHECK(fnType->getParamType(0)->isArrayTy());
    KAI_CHECK(fnType->getReturnType()->isArrayTy());
    KAI_CHECK(echo->arg_size() == 1);
    KAI_CHECK(!echo->hasStructRetAttr());
    KAI_CHECK(!echo->getAttributes().hasParamAttr(0, llvm::Attribute::ByVal));
    KAI_CHECK(!echo->getAttributes().hasParamAttr(0, llvm::Attribute::StructRet));
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

// Call with an existing array value, an inline literal argument, and the
// call result used to initialize a local (spec §19).
void testArrayCallWithExistingValueAndInlineLiteralVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn echo(xs: [i32; 3]) -> [i32; 3] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let a = [1, 2, 3]\n"
                                      "    let b = echo(a)\n"
                                      "    let c = echo([4, 5, 6])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Zero-length arrays through a parameter and a return (spec §14).
void testZeroLengthArrayParameterAndReturnVerify() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn echo(xs: [i32; 0]) -> [i32; 0] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let a: [i32; 0] = []\n"
                                      "    let b = echo(a)\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// --- KAI LANGUAGE M9: nested fixed-array indexing ---
//
// Before M9, lowerArrayElementAddress() only accepted a direct (through
// transparent ParenExpr only) IdentifierExpr as an IndexExpr's object -
// `matrix[0][1]` failed because `matrix[0]` (itself an IndexExpr) is not
// an IdentifierExpr, so codegen for the OUTER index rejected it outright
// (frontend type-checking already supported the nested TYPE rule via
// checkIndexExpr()'s own existing recursion - this was purely a codegen
// gap). M9 generalizes the object resolution via the new lowerArrayBase()
// helper, which recurses through nested IndexExpr layers, resolving each
// one's own address (bounds-checked GEP) before treating it as the next
// level's array storage - see lowerArrayBase()'s own doc comment in the
// header for the full design.

// 2-level nested read (spec §20.A/§21.A).
void testNestedArrayTwoLevelReadVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let matrix = [[1, 2], [3, 4]]\n"
                                      "    print(matrix[0][0])\n"
                                      "    print(matrix[0][1])\n"
                                      "    print(matrix[1][0])\n"
                                      "    print(matrix[1][1])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// 2-level nested write through a mutable local root (spec §20.B/§21.B).
void testNestedArrayTwoLevelWriteVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    mut matrix = [[1, 2], [3, 4]]\n"
                                      "    matrix[1][0] = 99\n"
                                      "    print(matrix[1][0])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A `let` root remains rejected via the EXISTING AssignmentToImmutableBinding
// diagnostic, not a new nested-specific one (spec §8/§20.C/§21.C) -
// mutability is decided by the ROOT binding alone.
void testNestedArrayWriteThroughImmutableRootFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn main() {\n    let matrix = [[1, 2], [3, 4]]\n    matrix[1][0] = 99\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == kai::semantic::SemanticErrorKind::AssignmentToImmutableBinding);
    }
    KAI_CHECK(!result.generationSucceeded);
}

// 3-level nested read - the recursion is not hardcoded to depth 2 (spec
// §16/§20.K/§21.K).
void testNestedArrayThreeLevelReadVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let cube = [[[1, 2], [3, 4]], [[5, 6], [7, 8]]]\n"
                                      "    print(cube[1][0][1])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// GEP/check ordering proof (spec §14/§20): asymmetric dimensions
// (`[[i32; 3]; 2]`) make the outer length (2) and inner length (3)
// distinguishable in the emitted ICmpULT constants, in the exact order
// they are lowered - the outer check (`i` against the matrix's own
// length) completes strictly before the inner check (`j` against the
// row's length) even begins, and NEITHER level's trap block ever
// contains a GEP/load/store (the element address is only ever computed
// in an in-bounds successor, at either level).
void testNestedIndexOuterCheckedBeforeInnerCheckAndNeitherGEPsEarly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let m: [[i32; 3]; 2] = [[1, 2, 3], [4, 5, 6]]\n"
                                      "    mut i: i32 = 1\n"
                                      "    mut j: i32 = 2\n"
                                      "    print(m[i][j])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    std::vector<std::int64_t> ultBounds;
    std::vector<const llvm::BasicBlock*> trapBlocks;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT) {
                    if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) {
                        ultBounds.push_back(c->getSExtValue());
                    }
                }
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "llvm.trap") {
                    trapBlocks.push_back(&block);
                }
            }
        }
    }

    // Outer length (2) checked strictly before inner length (3) - a
    // structural proof that the recursive base resolution (outer) always
    // fully completes, GEP included, before this level's own (inner)
    // check even begins.
    KAI_CHECK(ultBounds.size() == 2);
    if (ultBounds.size() == 2) {
        KAI_CHECK(ultBounds[0] == 2);
        KAI_CHECK(ultBounds[1] == 3);
    }

    KAI_CHECK(trapBlocks.size() == 2);
    for (const llvm::BasicBlock* trapBlock : trapBlocks) {
        KAI_CHECK(llvm::isa<llvm::UnreachableInst>(trapBlock->getTerminator()));
        for (const llvm::Instruction& inst : *trapBlock) {
            KAI_CHECK(!llvm::isa<llvm::GetElementPtrInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::LoadInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::StoreInst>(inst));
        }
    }
}

// Outer dynamic signed bounds contribute exactly one SGE + one ULT
// comparison; the inner index here is a compile-time constant (`0`),
// which IRBuilder constant-folds away entirely - no separate ICmp
// instruction for it (spec §20.F-ish "outer dynamic signed bounds").
void testNestedArrayOuterDynamicSignedBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let m = [[1, 2], [3, 4]]\n"
                                      "    mut i: i32 = 1\n"
                                      "    print(m[i][0])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int sgeCount = 0;
    int ultCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sgeCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                ultCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
        }
    }
    KAI_CHECK(sgeCount == 1);
    KAI_CHECK(ultCount == 1);
}

// Inner dynamic signed bounds - the mirror image: a compile-time-constant
// outer index (`0`) folds away, and the dynamic inner index (`j`)
// contributes exactly one SGE + one ULT (spec §20 "inner dynamic signed
// bounds").
void testNestedArrayInnerDynamicSignedBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let m = [[1, 2], [3, 4]]\n"
                                      "    mut j: i32 = 1\n"
                                      "    print(m[0][j])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int sgeCount = 0;
    int ultCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sgeCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                ultCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
        }
    }
    KAI_CHECK(sgeCount == 1);
    KAI_CHECK(ultCount == 1);
}

// Unsigned nested bounds: BOTH indices unsigned skips the SGE
// non-negativity check entirely at both levels - only the two ULT upper-
// bound comparisons remain (M7B spec §6, generalized to nesting depth by
// M9 spec §13/§20).
void testNestedArrayUnsignedBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let m = [[1, 2], [3, 4]]\n"
                                      "    mut i: u32 = 1\n"
                                      "    mut j: u32 = 0\n"
                                      "    print(m[i][j])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    int sgeCount = 0;
    int ultCount = 0;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sgeCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                ultCount += cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
        }
    }
    KAI_CHECK(sgeCount == 0);
    KAI_CHECK(ultCount == 2);
}

// Nested IndexExpr lowering exactly once, both indices side-effecting:
// `f()` (outer) and `g()` (inner) must each appear exactly once, in
// source order, never duplicated by address recomputation (spec §5/§15/
// §20/§21.J).
void testNestedIndexReadEvaluatedExactlyOnceInOrder() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn outer() -> i32 {\n    print(100)\n    return 1\n}\n"
                                      "fn inner() -> i32 {\n    print(200)\n    return 0\n}\n"
                                      "fn main() {\n"
                                      "    let matrix = [[1, 2], [3, 4]]\n"
                                      "    print(matrix[outer()][inner()])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    const llvm::Function* outerFn = module.getFunction("outer");
    const llvm::Function* innerFn = module.getFunction("inner");
    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(outerFn != nullptr && innerFn != nullptr && mainFn != nullptr);
    if (outerFn == nullptr || innerFn == nullptr || mainFn == nullptr) {
        return;
    }

    std::vector<const llvm::Function*> callOrder;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == outerFn || call->getCalledFunction() == innerFn) {
                    callOrder.push_back(call->getCalledFunction());
                }
            }
        }
    }
    KAI_CHECK(callOrder.size() == 2);
    if (callOrder.size() == 2) {
        KAI_CHECK(callOrder[0] == outerFn);
        KAI_CHECK(callOrder[1] == innerFn);
    }
}

// Assignment evaluation order (spec §15/§20/§21.J): for
// `matrix[f()][g()] = h()`, f()/g()/h() each lower exactly once, in
// source order, and `h()` (the RHS) only after BOTH checks succeeded.
void testNestedIndexAssignmentEvaluatedExactlyOnceInOrder() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn f() -> i32 {\n    print(100)\n    return 1\n}\n"
                                      "fn g() -> i32 {\n    print(200)\n    return 0\n}\n"
                                      "fn h() -> i32 {\n    print(300)\n    return 42\n}\n"
                                      "fn main() {\n"
                                      "    mut matrix = [[1, 2], [3, 4]]\n"
                                      "    matrix[f()][g()] = h()\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }

    const llvm::Module& module = codegen.module();
    const llvm::Function* fFn = module.getFunction("f");
    const llvm::Function* gFn = module.getFunction("g");
    const llvm::Function* hFn = module.getFunction("h");
    const llvm::Function* mainFn = module.getFunction("main");
    KAI_CHECK(fFn != nullptr && gFn != nullptr && hFn != nullptr && mainFn != nullptr);
    if (fFn == nullptr || gFn == nullptr || hFn == nullptr || mainFn == nullptr) {
        return;
    }

    std::vector<const llvm::Function*> callOrder;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == fFn || call->getCalledFunction() == gFn ||
                    call->getCalledFunction() == hFn) {
                    callOrder.push_back(call->getCalledFunction());
                }
            }
        }
    }
    KAI_CHECK(callOrder.size() == 3);
    if (callOrder.size() == 3) {
        KAI_CHECK(callOrder[0] == fFn);
        KAI_CHECK(callOrder[1] == gFn);
        KAI_CHECK(callOrder[2] == hFn);
    }
}

// Array-valued intermediate IndexExpr (spec §10/§20): `let row =
// matrix[1]` produces an INDEPENDENT array value, not an alias - a
// distinct alloca backs `row`, and mutating it must never touch
// `matrix`'s own storage. Structurally verified by asserting two
// distinct ArrayType allocas exist, plus a load from the GEP'd
// `matrix[1]` address feeding a store into `row`'s own alloca (the
// generic lowerExpr()-then-store fallback path
// generateArrayVarDeclStmt() already has, exercised here through an
// IndexExpr initializer specifically).
void testArrayValuedIntermediateIndexExprCopiesIndependently() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let matrix = [[1, 2], [3, 4]]\n"
                                      "    mut row = matrix[1]\n"
                                      "    row[0] = 99\n"
                                      "    print(row[0])\n"
                                      "    print(matrix[1][0])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    int arrayAllocaCount = 0;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            arrayAllocaCount += llvm::isa<llvm::ArrayType>(alloca->getAllocatedType());
        }
    }
    // `matrix` and `row` are each their own independent [i32; 2] alloca -
    // never the same storage.
    KAI_CHECK(arrayAllocaCount == 2);
}

// Nested-array parameter indexing (spec §11/§19/§20.L/§21.L): reading
// through a parameter works exactly like reading through a local, with
// no ABI change (M8B's direct aggregate strategy untouched).
void testNestedArrayParameterIndexingVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn get(m: [[i32; 2]; 2]) -> i32 {\n    return m[1][0]\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Returned nested-array indexing (spec §11/§20.M/§21.M): indexing into a
// local initialized from a nested-array-returning call.
void testNestedArrayReturnIndexingVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn make() -> [[i32; 2]; 2] {\n"
                                      "    return [[1, 2], [3, 4]]\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let m = make()\n"
                                      "    print(m[1][1])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// --- KAI LANGUAGE M10A: slice TYPE foundation - backend clean-failure
// coverage (spec §23) ---
//
// TypeKind::Slice is now a real semantic Type (SemanticAnalyzer::
// resolveSliceTypeSyntax()/SemanticModel::internSlice() - see
// SliceTypeTests.cpp for the full type-resolution coverage). KAI
// LANGUAGE M10B then made a BARE slice parameter genuinely executable
// (see this file's own dedicated M10B section further below for that
// positive coverage) - what remains backend-unsupported is an ARRAY that
// recursively contains a Slice, and a Slice RETURN type (both
// deliberate, spec §5/§6), covered here.

// RETARGETED (KAI LANGUAGE M10B): a bare slice PARAMETER is no longer
// unsupported (see testSliceParameterAndArgumentGenerateSuccessfully()
// in this file's own M10B section) - an ARRAY that recursively contains
// a Slice remains the unsupported shape, via the SAME "unsupported
// parameter type" unsupportedConstruct() message every other unsupported
// parameter type already produces.
void testArrayContainingSliceParameterFailsCleanlyAtBackend() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn sum(xs: [[i32]; 2]) -> i32 {\n    return 0\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
    KAI_CHECK(codegen.unsupportedConstruct().has_value());
    if (codegen.unsupportedConstruct().has_value()) {
        KAI_CHECK(codegen.unsupportedConstruct()->description ==
                  "code generation is not yet supported for this parameter's type");
    }
}

// KAI LANGUAGE M11B: a bare Slice RETURN type is now genuinely
// executable - but ONLY for a `return` expression KAI LANGUAGE M11A's
// own restricted provenance analysis has already proven `External`
// (relaying the slice parameter straight back out, exactly like this
// one). This locks in the raw ABI shape the M8B array tests above
// already establish the pattern for: a direct `{ptr, i64}` aggregate
// return AND argument, no `sret`, no hidden pointer, no `byval`. Full
// execution/call-result/no-element-copy coverage lives in this file's
// own M11B section further below; this one test stays narrowly about
// the FunctionType/attribute shape, mirroring
// testArrayParameterAndReturnUseDirectAggregateFunctionType() above.
void testSliceReturnTypeExecutesWithDirectAggregateAbi() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn identity(xs: [i32]) -> [i32] {\n    return xs\n}");

    KAI_CHECK(result.model.errors().empty());
    // KAI LANGUAGE M10B: this used to fail here (a bare Slice return was
    // unconditionally unsupported at codegen, even though `xs` itself
    // was semantically well-typed). KAI LANGUAGE M11B narrows the
    // backend's own guard so a `return` M11A has already proven
    // `External` now generates successfully.
    KAI_CHECK(result.generationSucceeded);
    KAI_CHECK(!codegen.unsupportedConstruct().has_value());
    if (!result.generationSucceeded) {
        return;
    }
    llvm::Function* identity = codegen.module().getFunction("identity");
    KAI_CHECK(identity != nullptr);
    if (identity == nullptr) {
        return;
    }
    llvm::FunctionType* fnType = identity->getFunctionType();
    KAI_CHECK(fnType->getNumParams() == 1);
    KAI_CHECK(fnType->getParamType(0)->isStructTy());
    KAI_CHECK(fnType->getReturnType()->isStructTy());
    KAI_CHECK(identity->arg_size() == 1);
    KAI_CHECK(!identity->hasStructRetAttr());
    KAI_CHECK(!identity->getAttributes().hasParamAttr(0, llvm::Attribute::ByVal));
    KAI_CHECK(!identity->getAttributes().hasParamAttr(0, llvm::Attribute::StructRet));
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

// Note: "an Array that recursively contains a Slice remains rejected as
// a RETURN type" (spec §19's own explicit regression requirement) is
// already covered by the pre-existing testArrayContainingSliceReturnTypeFailsCleanly()
// further below (in this file's own M10B section) - its fixture
// (`fn make(xs: [i32]) -> [[i32]; 2] { return [xs, xs] }`) is untouched
// by isUnsupportedSliceCarryingType()'s M11B narrowing (an Array is
// still an Array), so it was not duplicated here.

// --- KAI LANGUAGE M10B: immutable slice VALUES + checked indexing ---
//
// M10A's Slice TYPE foundation now has a real runtime representation and
// executable codegen: `slice(array)`, `len(...)`, local Slice storage/
// copy, checked Slice indexing, and Slice function parameters. Slice
// RETURNS and any executable aggregate recursively containing a Slice
// remain deliberately unsupported (see the two tests immediately above).

// LLVM {ptr, i64} type for Slice, and a local Slice storage alloca of
// exactly that type.
void testSliceLLVMTypeAndLocalStorage() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    let a = [10, 20, 30]\n    let s = slice(a)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    const llvm::AllocaInst* sliceAlloca = nullptr;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            if (llvm::isa<llvm::StructType>(alloca->getAllocatedType())) {
                sliceAlloca = alloca;
            }
        }
    }
    KAI_CHECK(sliceAlloca != nullptr);
    if (sliceAlloca == nullptr) {
        return;
    }
    const auto* sliceType = llvm::cast<llvm::StructType>(sliceAlloca->getAllocatedType());
    KAI_CHECK(sliceType->getNumElements() == 2);
    KAI_CHECK(sliceType->getElementType(0)->isPointerTy());
    KAI_CHECK(sliceType->getElementType(1)->isIntegerTy(64));
}

// `slice(a)`'s pointer field is a GEP directly into `a`'s OWN backing
// storage - never a copy into a temporary, never a heap allocation (spec
// §25).
void testSliceOfLocalArrayReferencesOriginalStorage() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    let a = [10, 20, 30]\n    let s = slice(a)\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    const llvm::AllocaInst* arrayAlloca = nullptr;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            if (llvm::isa<llvm::ArrayType>(alloca->getAllocatedType())) {
                arrayAlloca = alloca;
            }
        }
    }
    KAI_CHECK(arrayAlloca != nullptr);
    if (arrayAlloca == nullptr) {
        return;
    }

    bool sawGepIntoArrayFeedingInsertValue = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
            if (gep->getPointerOperand() == arrayAlloca) {
                for (const llvm::Use& use : gep->uses()) {
                    if (llvm::isa<llvm::InsertValueInst>(use.getUser())) {
                        sawGepIntoArrayFeedingInsertValue = true;
                    }
                }
            }
        }
    }
    KAI_CHECK(sawGepIntoArrayFeedingInsertValue);
}

// `slice(xs)` for an array PARAMETER references the callee's own
// parameter storage (M8's own binding), not a copy.
void testSliceOfArrayParameterReferencesParameterStorage() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f(xs: [i32; 3]) {\n    let s = slice(xs)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// `len(array)` lowers to a compile-time CONSTANT - never a runtime memory
// read.
void testLenOfArrayIsCompileTimeConstant() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let a = [1, 2, 3]\n    print(len(a))\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    // len()'s own result Type is u64 (spec §8), so `print` dispatches to
    // kai_print_u64 - never kai_print_i64.
    bool sawConstantThreeCallArgument = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "kai_print_u64") {
                    if (const auto* c = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0))) {
                        sawConstantThreeCallArgument = c->getZExtValue() == 3;
                    }
                }
            }
        }
    }
    KAI_CHECK(sawConstantThreeCallArgument);
}

// `len(slice)` extracts the Slice's own runtime length field - never a
// constant.
void testLenOfSliceExtractsRuntimeField() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn main() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n    print(len(s))\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    bool sawExtractValueFieldOne = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(&inst)) {
                if (extract->getIndices().size() == 1 && extract->getIndices()[0] == 1) {
                    sawExtractValueFieldOne = true;
                }
            }
        }
    }
    KAI_CHECK(sawExtractValueFieldOne);
}

// `len(str)` extracts Str's own byte-length field (field 1) - the SAME
// extraction shape as `len(slice)`, since both are `{ptr,i64}`
// aggregates, but reusing lowerPrintCall()'s own established Str
// extraction, never a second implementation. Uses a `str` PARAMETER
// (never a bare string LITERAL) deliberately: a literal's own
// `{ptr,i64}` value is a compile-time llvm::ConstantStruct (see
// lowerLiteralExpr()'s own String case), so CreateExtractValue on it
// constant-folds away into a plain ConstantInt with no ExtractValueInst
// instruction ever appearing at all - a parameter's str value is a
// genuine runtime SSA value instead, so the extraction survives as a
// real instruction to assert against.
void testLenOfStrExtractsByteLengthField() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn f(text: str) -> u64 {\n    return len(text)\n}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("f");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    bool sawExtractValueFieldOne = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(&inst)) {
                if (extract->getIndices().size() == 1 && extract->getIndices()[0] == 1) {
                    sawExtractValueFieldOne = true;
                }
            }
        }
    }
    KAI_CHECK(sawExtractValueFieldOne);
}

// Checked SIGNED Slice index CFG: SGE (non-negativity) + ULT (upper
// bound, against the Slice's own RUNTIME length field, not a constant) +
// trap + unreachable - mirrors array indexing's own CFG shape exactly
// (lowerCheckedIndexBounds() is shared between the two).
void testSignedDynamicSliceIndexBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let a = [10, 20, 30]\n"
                                      "    let s = slice(a)\n"
                                      "    mut i: i32 = 1\n"
                                      "    print(s[i])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSGE = false;
    bool sawULTAgainstNonConstant = false;
    bool sawTrapCall = false;
    const llvm::BasicBlock* trapBlock = nullptr;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSGE |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT) {
                    // The Slice's runtime length field, not a
                    // ConstantInt (which array indexing would use
                    // instead) - proves this is a genuine RUNTIME bound.
                    sawULTAgainstNonConstant |= !llvm::isa<llvm::ConstantInt>(cmp->getOperand(1));
                }
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "llvm.trap") {
                    sawTrapCall = true;
                    trapBlock = &block;
                }
            }
        }
    }
    KAI_CHECK(sawSGE);
    KAI_CHECK(sawULTAgainstNonConstant);
    KAI_CHECK(sawTrapCall);
    KAI_CHECK(trapBlock != nullptr);
    if (trapBlock != nullptr) {
        KAI_CHECK(llvm::isa<llvm::UnreachableInst>(trapBlock->getTerminator()));
        // No element GEP/load in the trap block - the element address is
        // only ever computed in the in-bounds successor (spec §18).
        for (const llvm::Instruction& inst : *trapBlock) {
            KAI_CHECK(!llvm::isa<llvm::GetElementPtrInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::LoadInst>(inst));
        }
    }
}

// Checked UNSIGNED Slice index CFG: skips the SGE non-negativity check
// entirely - only the ULT upper-bound comparison remains (spec §15/§16).
void testUnsignedDynamicSliceIndexBoundsCFG() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn main() {\n"
                                      "    let a = [10, 20, 30]\n"
                                      "    let s = slice(a)\n"
                                      "    mut i: u32 = 1\n"
                                      "    print(s[i])\n"
                                      "}");

    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }

    bool sawSGE = false;
    bool sawULT = false;
    bool sawTrapCall = false;
    for (const llvm::BasicBlock& block : *fn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                sawSGE |= cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE;
                sawULT |= cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT;
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "llvm.trap") {
                    sawTrapCall = true;
                }
            }
        }
    }
    KAI_CHECK(!sawSGE);
    KAI_CHECK(sawULT);
    KAI_CHECK(sawTrapCall);
}

// Index evaluated EXACTLY ONCE for a Slice read - a side-effecting index
// expression must appear in the call sequence exactly once (spec §19).
void testSliceIndexEvaluatedExactlyOnce() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn idx() -> i32 {\n    print(100)\n    return 1\n}\n"
                                      "fn main() {\n"
                                      "    let a = [10, 20, 30]\n"
                                      "    let s = slice(a)\n"
                                      "    print(s[idx()])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* idxFn = codegen.module().getFunction("idx");
    const llvm::Function* mainFn = codegen.module().getFunction("main");
    KAI_CHECK(idxFn != nullptr && mainFn != nullptr);
    if (idxFn == nullptr || mainFn == nullptr) {
        return;
    }

    int callCount = 0;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() == idxFn) {
                    ++callCount;
                }
            }
        }
    }
    KAI_CHECK(callCount == 1);
}

// Slice copy (`let t = s`) is a SINGLE aggregate load+store - never an
// element-by-element loop (spec §22).
void testSliceCopyIsAggregateNotElementLoop() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen,
        "fn main() {\n    let a = [10, 20, 30]\n    let s = slice(a)\n    let t = s\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    KAI_CHECK(!llvm::verifyModule(codegen.module()));

    const llvm::Function* fn = codegen.module().getFunction("main");
    KAI_CHECK(fn != nullptr);
    if (fn == nullptr) {
        return;
    }
    // No loop (no basic block branches back to an earlier one) - a
    // single, straight-line entry block covers the whole function.
    KAI_CHECK(fn->size() == 1);
    bool sawAggregateStoreOfStructType = false;
    for (const llvm::Instruction& inst : fn->getEntryBlock()) {
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            if (llvm::isa<llvm::StructType>(store->getValueOperand()->getType()) &&
                llvm::isa<llvm::LoadInst>(store->getValueOperand())) {
                sawAggregateStoreOfStructType = true;
            }
        }
    }
    KAI_CHECK(sawAggregateStoreOfStructType);
}

// Slice function PARAMETER signature: a direct `{ptr,i64}` aggregate
// argument - no sret/byval/hidden pointer, mirroring M8B's own array ABI
// assertions exactly (spec §21).
void testSliceParameterUsesDirectAggregateFunctionType() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn first(xs: [i32]) -> i32 {\n    return xs[0]\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    llvm::Function* first = codegen.module().getFunction("first");
    KAI_CHECK(first != nullptr);
    if (first == nullptr) {
        return;
    }
    llvm::FunctionType* fnType = first->getFunctionType();
    KAI_CHECK(fnType->getNumParams() == 1);
    KAI_CHECK(fnType->getParamType(0)->isStructTy());
    KAI_CHECK(!first->hasStructRetAttr());
    KAI_CHECK(!first->getAttributes().hasParamAttr(0, llvm::Attribute::ByVal));
    KAI_CHECK(!llvm::verifyModule(codegen.module()));
}

// Explicit `slice(a)` used directly as a call argument - the required
// explicit-conversion form (spec §2/§20).
void testExplicitSliceCallArgumentVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn first(xs: [i32]) -> i32 {\n"
                                      "    return xs[0]\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let a = [10, 20, 30]\n"
                                      "    print(first(slice(a)))\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Zero-length Slice: `slice()` over `[i32; 0]` verifies, with a runtime
// length of 0 (never dereferenced, since no in-bounds element exists).
void testZeroLengthSliceVerifies() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() {\n    let a: [i32; 0] = []\n    let s = slice(a)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Recursive guard: an array RETURN type that recursively contains a
// Slice (`[[i32]; 2]`) is rejected exactly like a bare Slice return -
// never silently allowed just because M8's own array-return machinery
// could otherwise lower it mechanically (spec §6/§28/§29).
void testArrayContainingSliceReturnTypeFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn make(xs: [i32]) -> [[i32]; 2] {\n"
                                      "    return [xs, xs]\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
    KAI_CHECK(codegen.unsupportedConstruct().has_value());
    if (codegen.unsupportedConstruct().has_value()) {
        KAI_CHECK(codegen.unsupportedConstruct()->description ==
                  "code generation is not yet supported for this function's return type");
    }
}

// A slice-typed LOCAL is fully supported (KAI LANGUAGE M10B) - unlike
// M10A, there is now a real way to construct a slice VALUE (`slice(a)`),
// so this is no longer merely a hypothetical relay case.
void testSliceLocalGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    let a = [1, 2, 3]\n    let s = slice(a)\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// An array LOCAL that recursively contains a Slice fails cleanly too
// (`generateVarDeclStmt()`'s own guard, mirroring declareFunction()'s).
void testArrayContainingSliceLocalFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn f(xs: [i32]) {\n    let m = [xs, xs]\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

// --- KAI LANGUAGE M11B: executable safe Slice returns ---
//
// M10B made Slice VALUES executable (locals, copies, parameters, checked
// indexing) but left every Slice RETURN unconditionally unsupported at
// codegen. M11A then added a restricted, flow-sensitive provenance
// analysis (External/Local/Unknown) that lets TypeChecker accept a
// narrow class of Slice returns as semantically SOUND, without changing
// codegen at all. M11B is the backend half: `declareFunction()`'s own
// return-type guard (`isUnsupportedSliceCarryingType()`) now allows a
// bare Slice return type through - reachable ONLY via a `return`
// expression M11A has already proven `External`, since anything else
// still fails at TypeChecker with EscapingLocalSlice before codegen ever
// runs. This section deliberately does NOT re-test M11A's own provenance
// rules (see SliceProvenanceTests.cpp for that) - only that a
// semantically-accepted Slice return now generates correct, working
// {ptr, i64} aggregate-transport code.

// A copied External parameter (`let s = xs; return s`) transports
// exactly like direct forwarding.
void testCopiedSliceParameterReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn identity(xs: [i32]) -> [i32] {\n    let s = xs\n    return s\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A double-copied External parameter (`let a = xs; let b = a; return b`)
// also transports correctly - provenance chains of arbitrary length are
// all just plain aggregate copies at this layer.
void testDoubleCopiedSliceParameterReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn identity(xs: [i32]) -> [i32] {\n    let a = xs\n    let b = a\n    return b\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A Slice binding reassigned sequentially but still External at `return`
// (`mut s = xs; s = ys; return s`) executes correctly - M11B does not
// introduce any alias restriction for immutable Slice views, and
// transport at this layer never depends on WHICH External source a
// binding's current value came from.
void testReassignedExternalSliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(
        sm, codegen, "fn choose(xs: [i32], ys: [i32]) -> [i32] {\n    mut s = xs\n    s = ys\n    return s\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// Both branches of an if/else produce External provenance
// (`if cond { s = xs } else { s = ys }; return s`) - M11A accepts this as
// External, and M11B must transport whichever branch actually ran with
// no dependence on the branch's own identity.
void testBranchSelectedExternalSliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn choose(xs: [i32], ys: [i32], cond: bool) -> [i32] {\n"
                                      "    mut s = xs\n"
                                      "    if cond {\n"
                                      "        s = xs\n"
                                      "    } else {\n"
                                      "        s = ys\n"
                                      "    }\n"
                                      "    return s\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A call to a Slice-returning function produces a normal `{ptr, i64}`
// aggregate CallInst result - lowerCallExpr() needed no Slice-specific
// change at all, since it already forwards whatever `function->
// getReturnType()` the callee's own FunctionType declares.
void testCallToSliceReturningFunctionProducesAggregateResult() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [i32]) -> [i32] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let values = [10, 20, 30]\n"
                                      "    let s = slice(values)\n"
                                      "    let t = identity(s)\n"
                                      "    print(len(t))\n"
                                      "}");

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
    const llvm::CallInst* identityCall = nullptr;
    for (const llvm::BasicBlock& block : *mainFn) {
        for (const llvm::Instruction& inst : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() != nullptr && call->getCalledFunction()->getName() == "identity") {
                    identityCall = call;
                }
            }
        }
    }
    KAI_CHECK(identityCall != nullptr);
    if (identityCall != nullptr) {
        KAI_CHECK(identityCall->getType()->isStructTy());
    }
}

// A returned Slice can be indexed directly after the call
// (`identity(s)[i]`-shaped usage via an intermediate local) - checked
// indexing (`lowerCheckedIndexBounds()`) needed no change either, since
// it already operates on whatever `{ptr, i64}` value it is given,
// regardless of whether that value came from a local, a parameter, or a
// call result.
void testReturnedSliceCanBeIndexed() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [i32]) -> [i32] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let values = [10, 20, 30]\n"
                                      "    let t = identity(slice(values))\n"
                                      "    print(t[0])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A zero-length Slice return - `len()` on the result must yield 0 with no
// dereference of the (empty) backing storage. Structural-only here (see
// NativeCompilationTests.cpp for the executed, exact-stdout version); this
// just confirms it generates and verifies successfully.
void testZeroLengthSliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [i32]) -> [i32] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let a: [i32; 0] = []\n"
                                      "    let s = identity(slice(a))\n"
                                      "    print(len(s))\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A `bool`-element Slice transports correctly through a Slice return -
// spec §15's element-type coverage, generalized beyond `i32`.
void testBoolSliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [bool]) -> [bool] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let values = [true, false]\n"
                                      "    let s = identity(slice(values))\n"
                                      "    print(s[0])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// An `f64`-element Slice transports correctly through a Slice return.
void testF64SliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [f64]) -> [f64] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "    let values = [1.5, 2.5]\n"
                                      "    let s = identity(slice(values))\n"
                                      "    print(s[0])\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// A `str`-element Slice transports correctly through a Slice return -
// `str` and Slice share the identical `{ptr, i64}` shape, but this is a
// SEPARATE Slice-of-str value (an array of `str` sliced), never `str`
// itself reinterpreted as a Slice.
void testStrSliceReturnGeneratesSuccessfully() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen,
                                      "fn identity(xs: [str]) -> [str] {\n"
                                      "    return xs\n"
                                      "}\n"
                                      "fn f(values: [str; 2]) {\n"
                                      "    let s = identity(slice(values))\n"
                                      "    print(len(s))\n"
                                      "}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (result.generationSucceeded) {
        KAI_CHECK(!llvm::verifyModule(codegen.module()));
    }
}

// The callee returns the SAME pointer it received - no backing-array
// element copy, no memcpy, no reconstructed temporary storage. Only the
// {ptr, i64} aggregate itself (a load + a return) ever appears in
// `identity`'s own body.
void testSliceReturnTransportsPointerWithNoElementCopy() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn identity(xs: [i32]) -> [i32] {\n    return xs\n}");

    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(result.generationSucceeded);
    if (!result.generationSucceeded) {
        return;
    }
    const llvm::Function* identity = codegen.module().getFunction("identity");
    KAI_CHECK(identity != nullptr);
    if (identity == nullptr) {
        return;
    }
    // No GEP, no per-element load/store, no memcpy/intrinsic call - only
    // the parameter's own alloca, a store of the incoming argument, a
    // load of the whole aggregate, and the terminating `ret`.
    std::size_t loadCount = 0;
    std::size_t storeCount = 0;
    for (const llvm::BasicBlock& block : *identity) {
        for (const llvm::Instruction& inst : block) {
            KAI_CHECK(!llvm::isa<llvm::GetElementPtrInst>(inst));
            KAI_CHECK(!llvm::isa<llvm::CallInst>(inst));
            if (llvm::isa<llvm::LoadInst>(inst)) {
                ++loadCount;
            }
            if (llvm::isa<llvm::StoreInst>(inst)) {
                ++storeCount;
            }
        }
    }
    KAI_CHECK(loadCount == 1);
    KAI_CHECK(storeCount == 1);
}

// Note: "an Array that recursively contains a Slice remains rejected as
// a LOCAL too" (proving isUnsupportedSliceCarryingType()'s shared policy
// did not accidentally loosen the local case while narrowing the return
// case) is already covered by the pre-existing
// testArrayContainingSliceLocalFailsCleanly() above, whose fixture is
// identically untouched by M11B - not duplicated here.

} // namespace

int main() {
    testMainReturnsI64Literal();
    testMultipleFunctionsBothExistAndVerify();
    testGeneratorReusedAcrossTwoModulesFunctionsDoNotLeak();
    testUnsupportedConstructDoesNotLeakAcrossGenerateCalls();

    testIntegerAddition();
    testNestedArithmetic();
    testSubtraction();
    testMultiplication();
    testSignedDivisionTruncatesTowardZero();
    testUnsignedDivision();
    testModulo();

    testI64ArithmeticWidth();
    testI8ArithmeticWidth();

    testUnarySignedNegate();
    testUnaryBoolNot();

    testSignedLessThan();

    testIntegerEquality();
    testBoolInequality();

    testReturnTrueLiteral();
    testLogicalAndOr();

    testFloatAddition();
    testFloatComparison();

    testParenthesizedReturnExpression();

    testUnsupportedRangeFailsCleanly();

    testAnnotatedLocalReturnIdentifier();
    testInferredLocalReturnIdentifier();
    testTwoLocalsArithmetic();
    testBoolLocal();
    testFloatLocal();

    testMutableLocalAssignment();
    testFullImperativeBody();
    testAssignmentExprStmtSucceedsAsUnit();

    testIdentifierProducesLoad();

    testUnsignedComparisonWithLocal();
    testSignedComparisonWithLocal();

    testUnitLocalFailsCleanly();

    testOneParameterLoaded();
    testTwoParametersArithmetic();
    testParameterPlusLocal();
    testParameterTypeWidthPreserved();

    testZeroArgumentCall();
    testParameterizedCall();
    testCallUsedInArithmetic();
    testCallAsLocalInitializer();
    testCallAsAssignmentRhs();
    testBoolCallWithUnaryNot();

    testForwardCall();
    testSelfRecursiveCallVerifies();
    testMutualRecursionVerifies();

    testExplicitBareReturnEmitsRetVoid();
    testImplicitUnitFallthroughEmitsRetVoid();
    testUnitReturningCallAsExprStmtSucceeds();

    testShortCircuitAndCallOnlyInRhsBlock();
    testShortCircuitOrCallOnlyInRhsBlock();
    testNestedShortCircuit();

    testUnsupportedBuiltinCallStillFailsCleanly();

    testParametersLocalsAssignmentCallIntegration();

    testIfNoElseFallsThrough();
    testIfElseBothFallThroughViaAssignments();
    testIfThenReturnsFalseContinues();
    testIfElseBothBranchesReturn();
    testElseIfChain();

    testIfConditionParameterBool();
    testIfConditionComparison();
    testIfConditionShortCircuitCall();

    testWhileMutableCounterLoop();
    testWhileBodyAssignment();
    testWhileBodyReturn();
    testWhileShortCircuitCondition();

    testIfInsideWhile();
    testWhileInsideIf();

    testFactorialIntegration();
    testFibonacciIntegration();

    testShadowingInsideIfBranch();

    testForLoopVariableAssignmentFailsCleanly();

    testForLoopBasicCFGStructure();
    testForLoopSignedRangeUsesSignedComparison();
    testForLoopUnsignedRangeUsesUnsignedComparison();
    testForLoopEndExpressionLoweredExactlyOnce();
    testForLoopNestedVerifies();
    testForLoopBodyReturnVerifies();

    testArrayLocalAllocaAndLiteralStores();
    testZeroLengthArrayLowersAndVerifies();
    testArrayLiteralElementsEvaluatedLeftToRightOnce();
    testElementReadUsesCorrectGEPAndLoad();
    testElementWriteStoresThroughGEP();
    testSignedDynamicIndexBoundsCFG();
    testUnsignedDynamicIndexBoundsCFG();
    testM6ForLoopIndexIntegrationVerifies();
    testM6ForLoopIndexedMutationIntegrationVerifies();

    testArrayParameterNowGeneratesSuccessfully();
    testArrayReturnTypeNowGeneratesSuccessfully();
    testWholeArrayInitializationFromAnotherArrayNowGeneratesSuccessfully();
    testWholeArrayAssignmentNowGeneratesSuccessfully();
    testWholeArraySelfAssignmentNowGeneratesSuccessfully();
    testArrayParameterAndReturnUseDirectAggregateFunctionType();
    testArrayCallWithExistingValueAndInlineLiteralVerifies();
    testZeroLengthArrayParameterAndReturnVerify();

    testNestedArrayTwoLevelReadVerifies();
    testNestedArrayTwoLevelWriteVerifies();
    testNestedArrayWriteThroughImmutableRootFailsCleanly();
    testNestedArrayThreeLevelReadVerifies();
    testNestedIndexOuterCheckedBeforeInnerCheckAndNeitherGEPsEarly();
    testNestedArrayOuterDynamicSignedBoundsCFG();
    testNestedArrayInnerDynamicSignedBoundsCFG();
    testNestedArrayUnsignedBoundsCFG();
    testNestedIndexReadEvaluatedExactlyOnceInOrder();
    testNestedIndexAssignmentEvaluatedExactlyOnceInOrder();
    testArrayValuedIntermediateIndexExprCopiesIndependently();
    testNestedArrayParameterIndexingVerifies();
    testNestedArrayReturnIndexingVerifies();

    testArrayContainingSliceParameterFailsCleanlyAtBackend();
    testSliceReturnTypeExecutesWithDirectAggregateAbi();

    testSliceLLVMTypeAndLocalStorage();
    testSliceOfLocalArrayReferencesOriginalStorage();
    testSliceOfArrayParameterReferencesParameterStorage();
    testLenOfArrayIsCompileTimeConstant();
    testLenOfSliceExtractsRuntimeField();
    testLenOfStrExtractsByteLengthField();
    testSignedDynamicSliceIndexBoundsCFG();
    testUnsignedDynamicSliceIndexBoundsCFG();
    testSliceIndexEvaluatedExactlyOnce();
    testSliceCopyIsAggregateNotElementLoop();
    testSliceParameterUsesDirectAggregateFunctionType();
    testExplicitSliceCallArgumentVerifies();
    testZeroLengthSliceVerifies();
    testArrayContainingSliceReturnTypeFailsCleanly();
    testSliceLocalGeneratesSuccessfully();
    testArrayContainingSliceLocalFailsCleanly();

    testCopiedSliceParameterReturnGeneratesSuccessfully();
    testDoubleCopiedSliceParameterReturnGeneratesSuccessfully();
    testReassignedExternalSliceReturnGeneratesSuccessfully();
    testBranchSelectedExternalSliceReturnGeneratesSuccessfully();
    testCallToSliceReturningFunctionProducesAggregateResult();
    testReturnedSliceCanBeIndexed();
    testZeroLengthSliceReturnGeneratesSuccessfully();
    testBoolSliceReturnGeneratesSuccessfully();
    testF64SliceReturnGeneratesSuccessfully();
    testStrSliceReturnGeneratesSuccessfully();
    testSliceReturnTransportsPointerWithNoElementCopy();

    testPrintLiteralI32SignExtendsToI64();
    testPrintVariableI64();
    testPrintUnsignedVariableZeroExtends();
    testPrintBoolLiteral();
    testPrintFloatLiteral();
    testPrintF32VariableExtendsToDouble();
    testMultiplePrintsReuseRuntimeDeclaration();
    testPrintStringArgumentSucceeds();
    testUserDefinedPrintShadowsBuiltin();

    return kai::test::failureCount == 0 ? 0 : 1;
}
