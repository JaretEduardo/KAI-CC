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
void testLogicalAndOr() {
    SourceManager sm;
    LLVMCodeGenerator codegenAnd(sm);
    Generated andResult = compileToLLVM(sm, codegenAnd, "fn main() -> bool {\n    return true && false\n}");
    KAI_CHECK(andResult.generationSucceeded);
    if (andResult.generationSucceeded) {
        const llvm::ConstantInt* c = returnedIntegerConstant(codegenAnd.module(), "main");
        KAI_CHECK(c != nullptr);
        if (c != nullptr) {
            KAI_CHECK(c->getZExtValue() == 0);
        }
    }

    SourceManager sm2;
    LLVMCodeGenerator codegenOr(sm2);
    Generated orResult = compileToLLVM(sm2, codegenOr, "fn main() -> bool {\n    return false || true\n}");
    KAI_CHECK(orResult.generationSucceeded);
    if (orResult.generationSucceeded) {
        const llvm::ConstantInt* c = returnedIntegerConstant(codegenOr.module(), "main");
        KAI_CHECK(c != nullptr);
        if (c != nullptr) {
            KAI_CHECK(c->getZExtValue() == 1);
        }
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

// A parameterized function still fails cleanly (M4 work) - a parameter's
// IdentifierExpr use has no storage slot in M3, exactly like an
// unresolved/unsupported symbol would.
void testParameterIdentifierStillFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result = compileToLLVM(sm, codegen, "fn identity(x: i64) -> i64 {\n    return x\n}");

    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
    KAI_CHECK(!result.generationSucceeded);
}

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

void testIfStmtStillFailsCleanly() {
    SourceManager sm;
    LLVMCodeGenerator codegen(sm);
    Generated result =
        compileToLLVM(sm, codegen, "fn main() -> i64 {\n    if true {\n        return 1\n    }\n    return 2\n}");

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
    testUnsupportedParameterFailsCleanly();

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
    testParameterIdentifierStillFailsCleanly();

    testUnsignedComparisonWithLocal();
    testSignedComparisonWithLocal();

    testUnitLocalFailsCleanly();
    testIfStmtStillFailsCleanly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
