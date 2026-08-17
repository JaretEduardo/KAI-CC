#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;

namespace {

void testAddVirtualFile() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("virtual.kai", "let x = 1\n");

    KAI_CHECK(file.isValid());
    KAI_CHECK(sm.fileName(file) == "virtual.kai");
    KAI_CHECK(sm.buffer(file) == "let x = 1\n");
}

void testMultipleFilesGetDistinctIds() {
    SourceManager sm;
    const FileId a = sm.addVirtualFile("a.kai", "aaa");
    const FileId b = sm.addVirtualFile("b.kai", "bbb");

    KAI_CHECK(a.isValid());
    KAI_CHECK(b.isValid());
    KAI_CHECK(!(a == b));
    KAI_CHECK(sm.buffer(a) == "aaa");
    KAI_CHECK(sm.buffer(b) == "bbb");
}

void testLineColumnSingleLine() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let x = 1");

    auto lc = sm.lineColumn(SourceLocation(file, 0));
    KAI_CHECK(lc.line == 1);
    KAI_CHECK(lc.column == 1);

    lc = sm.lineColumn(SourceLocation(file, 4));
    KAI_CHECK(lc.line == 1);
    KAI_CHECK(lc.column == 5);
}

void testLineColumnMultiLine() {
    SourceManager sm;
    const std::string source = "fn main() {\n    x()\n}\n";
    const FileId file = sm.addVirtualFile("a.kai", source);

    const auto secondLineStart = source.find("    x()");
    auto lc = sm.lineColumn(SourceLocation(file, static_cast<std::uint32_t>(secondLineStart)));
    KAI_CHECK(lc.line == 2);
    KAI_CHECK(lc.column == 1);

    const auto closingBrace = source.find('}');
    lc = sm.lineColumn(SourceLocation(file, static_cast<std::uint32_t>(closingBrace)));
    KAI_CHECK(lc.line == 3);
    KAI_CHECK(lc.column == 1);
}

void testLineTextStripsOnlyLineFeed() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "one\ntwo\nthree");

    KAI_CHECK(sm.lineText(file, 1) == "one");
    KAI_CHECK(sm.lineText(file, 2) == "two");
    KAI_CHECK(sm.lineText(file, 3) == "three");
}

void testLineTextStripsTrailingCarriageReturn() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "one\r\ntwo\r\n");

    KAI_CHECK(sm.lineText(file, 1) == "one");
    KAI_CHECK(sm.lineText(file, 2) == "two");

    // The underlying buffer is untouched: '\r' bytes are still present.
    KAI_CHECK(sm.buffer(file) == "one\r\ntwo\r\n");
}

void testTrailingNewlineProducesEmptyFinalLine() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abc\n");

    KAI_CHECK(sm.lineText(file, 1) == "abc");
    KAI_CHECK(sm.lineText(file, 2) == "");

    // The location immediately after the final '\n' is the EOF position,
    // which resolves to the empty line following it.
    const auto lc = sm.lineColumn(SourceLocation(file, 4));
    KAI_CHECK(lc.line == 2);
    KAI_CHECK(lc.column == 1);
}

void testSpanText() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "let value = 42");

    const SourceSpan span(SourceLocation(file, 4), SourceLocation(file, 9));
    KAI_CHECK(sm.text(span) == "value");
}

void testLoadFileReportsMissingFile() {
    SourceManager sm;
    const auto result = sm.loadFile(std::filesystem::path("this/file/does/not/exist.kai"));

    KAI_CHECK(!result.has_value());
}

void testLoadFileReadsRealFile() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "kai_source_manager_test.kai";

    {
        std::ofstream out(tempPath, std::ios::binary);
        out << "fn main() {}\n";
    }

    SourceManager sm;
    const auto result = sm.loadFile(tempPath);
    KAI_CHECK(result.has_value());
    if (result.has_value()) {
        KAI_CHECK(sm.buffer(*result) == "fn main() {}\n");
        KAI_CHECK(sm.fileName(*result) == tempPath.string());
    }

    std::error_code ec;
    fs::remove(tempPath, ec);
}

} // namespace

int main() {
    testAddVirtualFile();
    testMultipleFilesGetDistinctIds();
    testLineColumnSingleLine();
    testLineColumnMultiLine();
    testLineTextStripsOnlyLineFeed();
    testLineTextStripsTrailingCarriageReturn();
    testTrailingNewlineProducesEmptyFinalLine();
    testSpanText();
    testLoadFileReportsMissingFile();
    testLoadFileReadsRealFile();

    return kai::test::failureCount == 0 ? 0 : 1;
}
