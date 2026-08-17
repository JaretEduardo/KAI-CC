#include "kai/source/SourceLocation.hpp"
#include "kai/source/SourceManager.hpp"

#include "support/check.hpp"

using kai::FileId;
using kai::SourceLocation;
using kai::SourceManager;
using kai::SourceSpan;

namespace {

void testFileIdValidity() {
    FileId invalid;
    KAI_CHECK(!invalid.isValid());

    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abc");
    KAI_CHECK(file.isValid());
}

void testSourceLocationValidity() {
    SourceLocation invalid;
    KAI_CHECK(!invalid.isValid());

    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abc");
    const SourceLocation loc(file, 1);

    KAI_CHECK(loc.isValid());
    KAI_CHECK(loc.file() == file);
    KAI_CHECK(loc.offset() == 1);
}

void testSourceLocationEquality() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abc");

    const SourceLocation a(file, 2);
    const SourceLocation b(file, 2);
    const SourceLocation c(file, 3);

    KAI_CHECK(a == b);
    KAI_CHECK(!(a == c));

    // Default-constructed (invalid) locations compare equal to each other.
    KAI_CHECK(SourceLocation() == SourceLocation());
}

void testSourceSpanValidity() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abcdef");

    const SourceSpan valid(SourceLocation(file, 1), SourceLocation(file, 4));
    KAI_CHECK(valid.isValid());
    KAI_CHECK(valid.begin().offset() == 1);
    KAI_CHECK(valid.end().offset() == 4);

    const SourceSpan empty;
    KAI_CHECK(!empty.isValid());

    const SourceSpan point = SourceSpan::point(SourceLocation(file, 2));
    KAI_CHECK(point.isValid());
    KAI_CHECK(point.begin() == point.end());
}

void testSourceSpanEquality() {
    SourceManager sm;
    const FileId file = sm.addVirtualFile("a.kai", "abcdef");

    const SourceSpan a(SourceLocation(file, 0), SourceLocation(file, 3));
    const SourceSpan b(SourceLocation(file, 0), SourceLocation(file, 3));
    const SourceSpan c(SourceLocation(file, 1), SourceLocation(file, 3));

    KAI_CHECK(a == b);
    KAI_CHECK(!(a == c));
}

} // namespace

int main() {
    testFileIdValidity();
    testSourceLocationValidity();
    testSourceLocationEquality();
    testSourceSpanValidity();
    testSourceSpanEquality();

    return kai::test::failureCount == 0 ? 0 : 1;
}
