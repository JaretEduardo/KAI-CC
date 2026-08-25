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

void testBuiltinCallStillFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn main() {\n    print(1)\n}");

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

void testForStmtStillFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn f() -> i64 {\n    mut x: i64 = 0\n    for i in 0..10 {\n        x = i\n    }\n\n    return x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

} // namespace

int main() {
    testMainReturnsI64Literal();
    testMultipleFunctionsBothExistAndVerify();
    testGeneratorReusedAcrossTwoModulesFunctionsDoNotLeak();

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

    testBuiltinCallStillFailsCleanly();

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

    testForStmtStillFailsCleanly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
