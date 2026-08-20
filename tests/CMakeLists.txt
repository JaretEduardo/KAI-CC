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

kai_add_test(ast_node_tests ast/NodeTests.cpp)
kai_add_test(ast_expr_tests ast/ExprTests.cpp)
kai_add_test(ast_type_syntax_tests ast/TypeSyntaxTests.cpp)
kai_add_test(ast_stmt_tests ast/StmtTests.cpp)
kai_add_test(ast_decl_tests ast/DeclTests.cpp)
kai_add_test(ast_source_file_tests ast/SourceFileTests.cpp)

kai_add_test(parser_tests parser/ParserTests.cpp)

kai_add_test(semantic_type_tests semantic/TypeTests.cpp)
kai_add_test(semantic_model_tests semantic/SemanticModelTests.cpp)
kai_add_test(semantic_analyzer_tests semantic/SemanticAnalyzerTests.cpp)

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

add_executable(ast_printer_tests cli/AstPrinterTests.cpp)

target_link_libraries(ast_printer_tests
    PRIVATE
        kaicc_cli
)

target_include_directories(ast_printer_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

kai_enable_warnings(ast_printer_tests)

add_test(NAME ast_printer_tests COMMAND ast_printer_tests)
