#include "kai/ast/SourceFile.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <utility>
#include <vector>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;
using kai::ast::BlockPtr;
using kai::ast::BlockStmt;
using kai::ast::CallExpr;
using kai::ast::DeclPtr;
using kai::ast::Expr;
using kai::ast::ExprKind;
using kai::ast::ExprPtr;
using kai::ast::ExprStmt;
using kai::ast::FunctionDecl;
using kai::ast::Identifier;
using kai::ast::IdentifierExpr;
using kai::ast::LiteralExpr;
using kai::ast::LiteralKind;
using kai::ast::Param;
using kai::ast::SourceFile;
using kai::ast::StmtKind;
using kai::ast::StmtPtr;

namespace {

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

void testSourceFileOwnsAndMovesDeclarations() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() { }");

    const SourceSpan nameSpan = spanOf(file, 3, 7);
    const SourceSpan bodySpan = spanOf(file, 10, 13);
    const SourceSpan declSpan = spanOf(file, 0, 13);

    BlockPtr body = std::make_unique<BlockStmt>(std::vector<StmtPtr>{}, bodySpan);

    std::vector<DeclPtr> decls;
    decls.push_back(std::make_unique<FunctionDecl>(false, Identifier{nameSpan}, std::vector<Param>{}, nullptr,
                                                     std::move(body), declSpan));

    SourceFile sourceFile(file, std::move(decls), declSpan);

    KAI_CHECK(sourceFile.file() == file);
    KAI_CHECK(sourceFile.span() == declSpan);
    KAI_CHECK(sourceFile.declarations().size() == 1);
    KAI_CHECK(sourceFile.declarations()[0]->kind() == kai::ast::DeclKind::Function);

    // Ownership actually transferred out of the local vector.
    KAI_CHECK(decls.empty());
}

// Manually builds the full tree for:
//
//     fn main() {
//         print("Hello from KAI")
//     }
//
// and verifies it is navigable end-to-end and destroys cleanly (no
// Parser exists yet - this is the shape the first parser milestone is
// expected to produce).
void testHelloWorldShapedTreeConstructsAndDestroysCleanly() {
    const std::string source = "fn main() {\n    print(\"Hello from KAI\")\n}";
    SourceManager sm;
    const FileId file = sm.addVirtualFile("hello.kai", source);

    const SourceSpan fnNameSpan = spanOf(file, 3, 7);   // main
    const SourceSpan calleeSpan = spanOf(file, 16, 21); // print
    const SourceSpan argSpan = spanOf(file, 22, 38);    // "Hello from KAI"
    const SourceSpan callSpan = spanOf(file, 16, 39);   // print("Hello from KAI")
    const SourceSpan bodySpan = spanOf(file, 10, 41);   // { ... }
    const SourceSpan fnSpan = spanOf(file, 0, 41);      // whole function

    ExprPtr callee = std::make_unique<IdentifierExpr>(Identifier{calleeSpan}, calleeSpan);
    std::vector<ExprPtr> arguments;
    arguments.push_back(std::make_unique<LiteralExpr>(LiteralKind::String, argSpan));
    ExprPtr call = std::make_unique<CallExpr>(std::move(callee), std::move(arguments), callSpan);

    std::vector<StmtPtr> statements;
    statements.push_back(std::make_unique<ExprStmt>(std::move(call), callSpan));

    BlockPtr body = std::make_unique<BlockStmt>(std::move(statements), bodySpan);

    std::vector<DeclPtr> decls;
    decls.push_back(std::make_unique<FunctionDecl>(false, Identifier{fnNameSpan}, std::vector<Param>{}, nullptr,
                                                     std::move(body), fnSpan));

    SourceFile sourceFile(file, std::move(decls), fnSpan);

    KAI_CHECK(sourceFile.declarations().size() == 1);

    const auto& fn = static_cast<const FunctionDecl&>(*sourceFile.declarations()[0]);
    KAI_CHECK(sm.text(fn.name().span) == "main");
    KAI_CHECK(fn.params().empty());
    KAI_CHECK(fn.returnType() == nullptr);

    KAI_CHECK(fn.body().statements().size() == 1);
    const auto& stmt = *fn.body().statements()[0];
    KAI_CHECK(stmt.kind() == StmtKind::Expr);

    const auto& exprStmt = static_cast<const ExprStmt&>(stmt);
    KAI_CHECK(exprStmt.expr().kind() == ExprKind::Call);

    const auto& callExpr = static_cast<const CallExpr&>(exprStmt.expr());
    KAI_CHECK(callExpr.callee().kind() == ExprKind::Identifier);
    KAI_CHECK(sm.text(callExpr.callee().span()) == "print");

    KAI_CHECK(callExpr.arguments().size() == 1);
    KAI_CHECK(callExpr.arguments()[0]->kind() == ExprKind::Literal);
    KAI_CHECK(sm.text(callExpr.arguments()[0]->span()) == "\"Hello from KAI\"");

    // sourceFile goes out of scope here, recursively destroying the
    // whole unique_ptr-owned tree.
}

} // namespace

int main() {
    testSourceFileOwnsAndMovesDeclarations();
    testHelloWorldShapedTreeConstructsAndDestroysCleanly();

    return kai::test::failureCount == 0 ? 0 : 1;
}
