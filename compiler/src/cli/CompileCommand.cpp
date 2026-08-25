#include "kai/cli/CompileCommand.hpp"

#include "kai/ast/SourceFile.hpp"
#include "kai/cli/AstPrinter.hpp"
#include "kai/codegen/LLVMCodeGenerator.hpp"
#include "kai/codegen/LLVMObjectEmitter.hpp"
#include "kai/codegen/NativeLinker.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/ControlFlowAnalyzer.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/TypeChecker.hpp"

#include <llvm/IR/Module.h>

#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace kai::cli {

namespace {

// Minimal, single-line SemanticError formatting - CLI-only text, the same
// "temporary, not a Diagnostic" spirit as AstPrinter.cpp's own
// formatParseError() (M7 spec §18: use existing diagnostics
// infrastructure as-is, do not redesign it here). No message string
// exists on SemanticError itself (see SemanticModel.hpp's own class
// comment) - this only renders the structured kind/location it does
// carry.
const char* semanticErrorKindName(semantic::SemanticErrorKind kind) {
    switch (kind) {
        case semantic::SemanticErrorKind::DuplicateSymbol:
            return "duplicate symbol";
        case semantic::SemanticErrorKind::UnknownIdentifier:
            return "unknown identifier";
        case semantic::SemanticErrorKind::UnknownType:
            return "unknown type";
        case semantic::SemanticErrorKind::TypeMismatch:
            return "type mismatch";
        case semantic::SemanticErrorKind::LiteralOutOfRange:
            return "literal out of range";
        case semantic::SemanticErrorKind::InvalidUnaryOperand:
            return "invalid unary operand";
        case semantic::SemanticErrorKind::InvalidBinaryOperands:
            return "invalid binary operands";
        case semantic::SemanticErrorKind::InvalidArgumentCount:
            return "invalid argument count";
        case semantic::SemanticErrorKind::NotCallable:
            return "not callable";
        case semantic::SemanticErrorKind::InvalidAssignmentTarget:
            return "invalid assignment target";
        case semantic::SemanticErrorKind::AssignmentToImmutableBinding:
            return "assignment to immutable binding";
        case semantic::SemanticErrorKind::MissingReturn:
            return "missing return";
    }
    return "semantic error";
}

std::string formatSemanticError(const SourceManager& sources, const semantic::SemanticError& error) {
    const SourceManager::LineColumn where = sources.lineColumn(error.primarySpan.begin());
    std::ostringstream message;
    message << "kaicc: error at " << where.line << ':' << where.column << ": " << semanticErrorKindName(error.kind);
    return message.str();
}

} // namespace

int runCompileCommand(SourceManager& sources, const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath, std::ostream& err) {
    const auto loaded = sources.loadFile(inputPath);
    if (!loaded) {
        err << "kaicc: error: failed to load '" << inputPath.string() << "': " << loaded.error().message() << '\n';
        return 2;
    }

    parser::Parser parser(sources, *loaded);
    const auto parsed = parser.parseSourceFile();
    if (!parsed) {
        err << formatParseError(sources, parsed.error()) << '\n';
        return 4;
    }

    semantic::SemanticModel model;
    semantic::SemanticAnalyzer analyzer(sources);
    model = analyzer.analyze(*parsed);

    semantic::TypeChecker checker(sources);
    checker.check(*parsed, model);

    const semantic::ControlFlowAnalyzer flow;
    flow.check(*parsed, model);

    if (!model.errors().empty()) {
        for (const semantic::SemanticError& error : model.errors()) {
            err << formatSemanticError(sources, error) << '\n';
        }
        return 5;
    }

    codegen::LLVMCodeGenerator generator(sources);
    if (!generator.generate(*parsed, model)) {
        err << "kaicc: error: LLVM IR generation failed\n";
        return 6;
    }

    llvm::Module& module = generator.module();

    if (!codegen::LLVMObjectEmitter::adaptNativeEntryPoint(module, err)) {
        return 7;
    }

    codegen::LLVMObjectEmitter::initializeNativeTarget();

    std::filesystem::path objectPath = outputPath;
    objectPath += ".o";

    // Best-effort cleanup of the intermediate object file on every exit
    // path below (M7 spec §20) - no temp-file framework, just removing
    // whatever this function itself created.
    const auto cleanupObject = [&objectPath]() {
        std::error_code ignored;
        std::filesystem::remove(objectPath, ignored);
    };

    if (!codegen::LLVMObjectEmitter::emit(module, objectPath, err)) {
        cleanupObject();
        return 8;
    }

    const std::optional<std::string> compilerDriver = codegen::NativeLinker::findCompilerDriver();
    if (!compilerDriver.has_value()) {
        err << "kaicc: error: no usable host C compiler driver found (tried $KAI_CC, cc, clang, gcc)\n";
        cleanupObject();
        return 9;
    }

    const std::filesystem::path kaiccPath = codegen::NativeLinker::currentExecutablePath();
    const std::optional<std::filesystem::path> runtimeLibrary =
        codegen::NativeLinker::findDefaultRuntimeLibrary(kaiccPath);
    if (!runtimeLibrary.has_value()) {
        err << "kaicc: error: could not locate the default KAI runtime library (libkai_runtime.a) - set "
               "KAI_RUNTIME_LIB to override\n";
        cleanupObject();
        return 10;
    }

    if (!codegen::NativeLinker::link(*compilerDriver, objectPath, *runtimeLibrary, outputPath, err)) {
        std::error_code ignored;
        std::filesystem::remove(outputPath, ignored); // no partial/stale executable left behind
        cleanupObject();
        return 11;
    }

    cleanupObject();
    return 0;
}

} // namespace kai::cli
