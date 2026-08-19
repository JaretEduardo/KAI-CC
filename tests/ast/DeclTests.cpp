#include "kai/ast/Decl.hpp"
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
using kai::ast::DeclKind;
using kai::ast::FunctionDecl;
using kai::ast::Identifier;
using kai::ast::NamedTypeSyntax;
using kai::ast::Param;
using kai::ast::StmtPtr;
using kai::ast::TypeSyntaxPtr;

namespace {

SourceSpan spanOf(FileId file, std::uint32_t begin, std::uint32_t end) {
    return SourceSpan(SourceLocation(file, begin), SourceLocation(file, end));
}

void testFunctionDeclWithEmptyParamsAndAbsentReturnType() {
    // fn main() { }
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "fn main() { }");

    const SourceSpan nameSpan = spanOf(file, 3, 7);   // main
    const SourceSpan bodySpan = spanOf(file, 10, 13); // { }
    const SourceSpan declSpan = spanOf(file, 0, 13);  // fn main() { }

    BlockPtr body = std::make_unique<BlockStmt>(std::vector<StmtPtr>{}, bodySpan);

    FunctionDecl fn(/*isPublic=*/false, Identifier{nameSpan}, std::vector<Param>{}, /*returnType=*/nullptr,
                     std::move(body), declSpan);

    KAI_CHECK(fn.kind() == DeclKind::Function);
    KAI_CHECK(fn.span() == declSpan);
    KAI_CHECK(!fn.isPublic());
    KAI_CHECK(fn.name().span == nameSpan);
    KAI_CHECK(sm.text(fn.name().span) == "main");
    KAI_CHECK(fn.params().empty());
    KAI_CHECK(fn.returnType() == nullptr);
    KAI_CHECK(fn.body().span() == bodySpan);
    KAI_CHECK(fn.body().statements().empty());
    KAI_CHECK(body == nullptr);
}

void testFunctionDeclWithParamsAndReturnTypeAndVisibility() {
    // pub fn add(a: i32, b: i32) -> i32 { }
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "pub fn add(a: i32, b: i32) -> i32 { }");

    const SourceSpan fnNameSpan = spanOf(file, 7, 10); // add

    const SourceSpan aNameSpan = spanOf(file, 11, 12); // a
    const SourceSpan aTypeSpan = spanOf(file, 14, 17); // i32
    const SourceSpan aParamSpan = spanOf(file, 11, 17);

    const SourceSpan bNameSpan = spanOf(file, 19, 20); // b
    const SourceSpan bTypeSpan = spanOf(file, 22, 25); // i32
    const SourceSpan bParamSpan = spanOf(file, 19, 25);

    const SourceSpan returnTypeSpan = spanOf(file, 30, 33); // i32
    const SourceSpan bodySpan = spanOf(file, 34, 37);        // { }
    const SourceSpan declSpan = spanOf(file, 0, 37);

    std::vector<Param> params;
    params.push_back(Param{Identifier{aNameSpan}, std::make_unique<NamedTypeSyntax>(Identifier{aTypeSpan}, aTypeSpan),
                            aParamSpan});
    params.push_back(Param{Identifier{bNameSpan}, std::make_unique<NamedTypeSyntax>(Identifier{bTypeSpan}, bTypeSpan),
                            bParamSpan});

    TypeSyntaxPtr returnType = std::make_unique<NamedTypeSyntax>(Identifier{returnTypeSpan}, returnTypeSpan);
    BlockPtr body = std::make_unique<BlockStmt>(std::vector<StmtPtr>{}, bodySpan);

    FunctionDecl fn(/*isPublic=*/true, Identifier{fnNameSpan}, std::move(params), std::move(returnType),
                     std::move(body), declSpan);

    KAI_CHECK(fn.isPublic());
    KAI_CHECK(sm.text(fn.name().span) == "add");

    KAI_CHECK(fn.params().size() == 2);
    KAI_CHECK(sm.text(fn.params()[0].name.span) == "a");
    KAI_CHECK(sm.text(fn.params()[1].name.span) == "b");

    KAI_CHECK(fn.returnType() != nullptr);
    KAI_CHECK(fn.returnType()->span() == returnTypeSpan);
    KAI_CHECK(sm.text(fn.returnType()->span()) == "i32");
}

} // namespace

int main() {
    testFunctionDeclWithEmptyParamsAndAbsentReturnType();
    testFunctionDeclWithParamsAndReturnTypeAndVisibility();

    return kai::test::failureCount == 0 ? 0 : 1;
}
