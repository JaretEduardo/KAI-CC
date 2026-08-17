# Registers a test executable that links kai_compiler and runs under CTest.
#
# No test framework has been selected for KAI-CC yet, so these tests use a
# tiny in-repo CHECK macro (tests/support/check.hpp) instead of pulling in
# an external dependency.
function(kai_add_test name)
    add_executable(${name} ${ARGN})

    target_link_libraries(${name}
        PRIVATE
            kai_compiler
    )

    target_include_directories(${name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    kai_enable_warnings(${name})

    add_test(NAME ${name} COMMAND ${name})
endfunction()

kai_add_test(source_location_tests source/SourceLocationTests.cpp)
kai_add_test(source_manager_tests source/SourceManagerTests.cpp)

kai_add_test(token_kind_tests lexer/TokenKindTests.cpp)
kai_add_test(token_tests lexer/TokenTests.cpp)
kai_add_test(lexer_tests lexer/LexerTests.cpp)

# kaicc_cli-specific tests: linked against kaicc_cli (not kai_compiler
# directly) since kai_add_test() always links kai_compiler and
# kaicc_cli's TokenPrinter helpers live one layer above that.
add_executable(token_printer_tests cli/TokenPrinterTests.cpp)

target_link_libraries(token_printer_tests
    PRIVATE
        kaicc_cli
)

target_include_directories(token_printer_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

kai_enable_warnings(token_printer_tests)

add_test(NAME token_printer_tests COMMAND token_printer_tests)
