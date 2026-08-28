#include "kai/Version.hpp"

#include "support/check.hpp"

// KAI v0.1.0-alpha.2 FINAL PRE-RELEASE PREPARATION: kai::kVersion is the
// one canonical compiler version string (PROJECT_VERSION + prerelease
// suffix, combined by CMake's configure_file() into the generated
// kai/Version.hpp - see CMakeLists.txt). This test protects it from
// silently drifting from the value main.cpp's --version actually prints.
void testVersionStringMatchesCurrentPreRelease() {
    KAI_CHECK(kai::kVersion == "0.1.0-alpha.2");
}

int main() {
    testVersionStringMatchesCurrentPreRelease();
    return kai::test::failureCount == 0 ? 0 : 1;
}
