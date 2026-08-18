cmake_minimum_required(VERSION 3.20)

project(
    KAI_CC
    VERSION 0.1.0
    DESCRIPTION "Reference compiler for the KAI programming language"
    LANGUAGES CXX
)

# C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Applies the project's standard compiler warnings to a target.
function(kai_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

# Shared compiler infrastructure library.
#
# Currently contains only source management. Future frontend/middle-end
# stages (lexer, parser, ast, semantic analysis, hir, codegen, diagnostics)
# will be added here as they are implemented. Both kaicc and the test
# suite link against this library rather than compiling sources directly,
# so the same code is exercised by both.
add_library(kai_compiler STATIC
    compiler/src/source/SourceManager.cpp
    compiler/src/lexer/TokenKind.cpp
    compiler/src/lexer/Lexer.cpp
    compiler/src/parser/Parser.cpp
)

# Public compiler headers
target_include_directories(kai_compiler
    PUBLIC
        ${PROJECT_SOURCE_DIR}/compiler/include
)

kai_enable_warnings(kai_compiler)

# CLI-only formatting/orchestration helpers (e.g. the --tokens debug dump).
#
# Kept separate from kai_compiler so the compiler/frontend core never
# acquires CLI presentation responsibilities. Dependency direction is
# strictly kaicc -> kaicc_cli -> kai_compiler, never the reverse.
add_library(kaicc_cli STATIC
    compiler/src/cli/TokenPrinter.cpp
)

target_include_directories(kaicc_cli
    PUBLIC
        ${PROJECT_SOURCE_DIR}/compiler/include
)

target_link_libraries(kaicc_cli
    PUBLIC
        kai_compiler
)

kai_enable_warnings(kaicc_cli)

# Main compiler executable
add_executable(kaicc
    compiler/src/main.cpp
)

target_link_libraries(kaicc
    PRIVATE
        kaicc_cli
)

kai_enable_warnings(kaicc)

# Tests
enable_testing()
add_subdirectory(tests)