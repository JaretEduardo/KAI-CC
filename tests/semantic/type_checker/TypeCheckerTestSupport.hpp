#pragma once

// Shared test-only infrastructure for the TypeChecker test suite, split
// across tests/semantic/type_checker/*.cpp by milestone (see
// LiteralAndInferenceTests.cpp, OperatorTests.cpp, CallTests.cpp,
// AssignmentTests.cpp, ConditionAndReturnTests.cpp). Extracted verbatim
// from the original single-file TypeCheckerTests.cpp so every split file
// shares one definition of the pipeline helper and the AST/semantic type
// aliases their test bodies rely on.

#include "kai/semantic/TypeChecker.hpp"

#include "kai/ast/Decl.hpp"
#include "kai/ast/Expr.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/parser/Parser.hpp"
#include "kai/semantic/SemanticAnalyzer.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <string>
#include <utility>

namespace kai::test::type_checker {

using kai::FileId;
using kai::SourceManager;
using kai::ast::ArrayLiteralExpr;
using kai::ast::AssignmentExpr;
using kai::ast::BinaryExpr;
using kai::ast::CallExpr;
using kai::ast::ErrorPropagationExpr;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::IdentifierExpr;
using kai::ast::IndexExpr;
using kai::ast::MemberExpr;
using kai::ast::ParenExpr;
using kai::ast::ReturnStmt;
using kai::ast::UnaryExpr;
using kai::ast::VarDeclStmt;
using kai::parser::ParseResult;
using kai::parser::Parser;
using kai::semantic::SemanticAnalyzer;
using kai::semantic::SemanticErrorKind;
using kai::semantic::SemanticModel;
using kai::semantic::Type;
using kai::semantic::TypeChecker;

// Mirrors SemanticAnalyzerTests.cpp's own Analyzed bundle, extended with
// the TypeChecker pass run on top of SemanticAnalyzer's output - the
// exact pipeline Milestone 1's spec requires: SourceManager -> Parser ->
// SemanticAnalyzer -> SemanticModel -> TypeChecker -> query mutated
// SemanticModel.
struct Checked {
    ParseResult<kai::ast::SourceFile> parsed;
    SemanticModel model;
};

inline Checked analyzeAndCheck(SourceManager& sm, const std::string& source) {
    const FileId file = sm.addVirtualFile("a.kai", source);
    Parser parser(sm, file);
    auto parsed = parser.parseSourceFile();

    SemanticModel model;
    if (parsed.has_value()) {
        SemanticAnalyzer analyzer(sm);
        model = analyzer.analyze(*parsed);

        TypeChecker checker(sm);
        checker.check(*parsed, model);
    }

    return Checked{std::move(parsed), std::move(model)};
}

} // namespace kai::test::type_checker
