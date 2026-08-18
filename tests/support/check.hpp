#pragma once

#include <iostream>
#include <string_view>

// A minimal check macro used by KAI-CC's unit tests. KAI-CC has not yet
// selected a test framework, so tests avoid an external dependency and
// instead report failures through this header and a non-zero exit code,
// which is all CTest needs.

namespace kai::test {

inline int failureCount = 0;

inline void reportFailure(std::string_view expr, std::string_view file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << '\n';
    ++failureCount;
}

} // namespace kai::test

#define KAI_CHECK(expr)                                                                                    \
    do {                                                                                                    \
        if (!(expr)) {                                                                                       \
            ::kai::test::reportFailure(#expr, __FILE__, __LINE__);                                            \
        }                                                                                                    \
    } while (false)
