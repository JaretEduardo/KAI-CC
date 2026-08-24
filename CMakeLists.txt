cmake_minimum_required(VERSION 3.20)

project(
    KAI_CC
    VERSION 0.1.0
    DESCRIPTION "Reference compiler for the KAI programming language"
    LANGUAGES CXX C
)
# C is enabled only because LLVMConfig.cmake's own optional dependency
# probing (e.g. LibEdit) uses try_compile() with a .c file; KAI-CC itself
# has no C sources.

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

# LLVM backend (LLVM CODEGEN MILESTONE 1). AST + SemanticModel lower
# directly to LLVM IR for the MVP - no HIR/MIR stage. Uses LLVM's own
# installed CMake package rather than any hard-coded path, so this
# resolves correctly on whatever LLVM version is actually installed.
find_package(LLVM CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

# LLVM_DEFINITIONS is a single space-separated string, not a CMake list -
# split it before handing it to target_compile_definitions().
separate_arguments(KAI_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# Only the one LLVM component this milestone actually needs - not every
# LLVM library.
llvm_map_components_to_libnames(KAI_LLVM_LIBS core)

# Shared compiler infrastructure library.
#
# Frontend stages (lexer, parser, ast, semantic analysis) plus the LLVM
# codegen backend (see compiler/include/kai/codegen). HIR/MIR are not
# part of the MVP path. Both kaicc and the test suite link against this
# library rather than compiling sources directly, so the same code is
# exercised by both.
add_library(kai_compiler STATIC
    compiler/src/source/SourceManager.cpp
    compiler/src/lexer/TokenKind.cpp
    compiler/src/lexer/Lexer.cpp
    compiler/src/parser/Parser.cpp
    compiler/src/semantic/SemanticAnalyzer.cpp
    compiler/src/semantic/TypeChecker.cpp
    compiler/src/semantic/ControlFlowAnalyzer.cpp
    compiler/src/codegen/LLVMCodeGenerator.cpp
    compiler/src/codegen/LLVMExpressionLowering.cpp
)

# Public compiler headers
target_include_directories(kai_compiler
    PUBLIC
        ${PROJECT_SOURCE_DIR}/compiler/include
)

# LLVM headers are third-party: included as SYSTEM so their own warnings
# never trigger under this project's -Wall -Wextra -Wpedantic, without
# weakening those flags for our own code. PUBLIC because
# LLVMCodeGenerator.hpp itself includes LLVM headers, so every consumer
# of kai_compiler (kaicc_cli, the test suite) needs them too.
target_include_directories(kai_compiler
    SYSTEM PUBLIC
        ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(kai_compiler
    PUBLIC
        ${KAI_LLVM_DEFINITIONS}
)

target_link_libraries(kai_compiler
    PUBLIC
        ${KAI_LLVM_LIBS}
)

kai_enable_warnings(kai_compiler)

# CLI-only formatting/orchestration helpers (e.g. the --tokens debug dump).
#
# Kept separate from kai_compiler so the compiler/frontend core never
# acquires CLI presentation responsibilities. Dependency direction is
# strictly kaicc -> kaicc_cli -> kai_compiler, never the reverse.
add_library(kaicc_cli STATIC
    compiler/src/cli/TokenPrinter.cpp
    compiler/src/cli/AstPrinter.cpp
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