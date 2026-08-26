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

# LLVM CODEGEN MILESTONE 7 needs more than bare IR construction (`core`):
# native object emission (LLVMObjectEmitter) and process-spawning link
# invocation (NativeLinker) pull in the host-native target backend and
# LLVM's own object/MC/support layers. `native`/`nativecodegen` are
# LLVM's own "whichever backend this build was configured for" meta-
# components (see llvm-config --components) - this deliberately avoids
# hard-coding e.g. `X86CodeGen`, and avoids linking every architecture
# LLVM can target, while still resolving to the real target-specific
# libraries this host actually needs.
llvm_map_components_to_libnames(KAI_LLVM_LIBS core native nativecodegen mc object support target)

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
    compiler/src/semantic/SemanticTypeName.cpp
    compiler/src/semantic/SemanticInspector.cpp
    compiler/src/semantic/SemanticInspectionJson.cpp
    compiler/src/semantic/SemanticQuery.cpp
    compiler/src/semantic/SemanticQueryJson.cpp
    compiler/src/semantic/SemanticCallQuery.cpp
    compiler/src/semantic/SemanticCallQueryJson.cpp
    compiler/src/codegen/LLVMCodeGenerator.cpp
    compiler/src/codegen/LLVMExpressionLowering.cpp
    compiler/src/codegen/LLVMObjectEmitter.cpp
    compiler/src/codegen/NativeLinker.cpp
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
    compiler/src/cli/SemanticErrorFormat.cpp
    compiler/src/cli/CompileCommand.cpp
    compiler/src/cli/InspectCommand.cpp
    compiler/src/cli/SemanticQueryCommand.cpp
    compiler/src/cli/SemanticCallQueryCommand.cpp
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

# Tiny native runtime KAI-generated LLVM IR calls into (LLVM CODEGEN
# MILESTONE 6 - see runtime/kai_runtime.h for the exact exported C ABI
# and compiler/include/kai/codegen/LLVMCodeGenerator.hpp's lowerPrintCall()
# doc comment for how generated modules reference it). Deliberately its
# own target, separate from kai_compiler/kaicc: the compiler process
# itself never calls into this library - it only emits `declare`/`call`
# references to these symbols by name. M7 links this runtime's object
# code into every generated native executable (see NativeLinker).
add_library(kai_runtime STATIC
    runtime/kai_runtime.c
)

target_include_directories(kai_runtime
    PUBLIC
        ${PROJECT_SOURCE_DIR}/runtime
)

kai_enable_warnings(kai_runtime)

# LLVM CODEGEN MILESTONE 7: arrange build outputs to already look like the
# eventual packaged/installed shape (`bin/kaicc` + `lib/kai/
# libkai_runtime.a`), rather than CMake's default flat build directory -
# this is exactly the layout NativeLinker::findDefaultRuntimeLibrary()
# looks for relative to kaicc's OWN running executable path, so the same
# relocatable lookup already works unmodified in this build tree, with no
# separate build-tree-only fallback needed (see NativeLinker.hpp).
set_target_properties(kaicc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
set_target_properties(kai_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/kai
)

# RELEASE HARDENING M1: install rules for the two artifacts an end user
# actually needs (`kaicc`, `libkai_runtime.a`) - this is currently a
# runtime/compiler DISTRIBUTION, not an SDK, so no headers, no LLVM
# headers, no tests, no editor files are installed.
#
# `kaicc` uses GNUInstallDirs' CMAKE_INSTALL_BINDIR (always "bin" on every
# platform this project targets). The runtime archive deliberately does
# NOT use CMAKE_INSTALL_LIBDIR: that variable resolves to "lib64" on
# Fedora/RHEL-family hosts (verified) but NativeLinker::
# findDefaultRuntimeLibrary() (NativeLinker.cpp) has a HARD-CODED lookup
# of `<kaicc's own directory>/../lib/kai/libkai_runtime.a` - literally
# "lib", never "lib64" - so the install destination is the literal string
# "lib/kai" on every platform, keeping the installed tree byte-for-byte
# identical to what NativeLinker already looks for, regardless of this
# platform's own GNUInstallDirs convention.
include(GNUInstallDirs)

install(TARGETS kaicc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES $<TARGET_FILE:kai_runtime>
    DESTINATION lib/kai
)

# RELEASE HARDENING M1.1: some LLVM distributions (Ubuntu 22.04's
# apt.llvm.org packages, confirmed) build LLVMSupport with
# LLVM_ENABLE_Z3_SOLVER=ON, which bakes an unconditional dependency on the
# system's libz3.so directly into LLVMSupport's own exported
# INTERFACE_LINK_LIBRARIES (LLVMExports.cmake) - confirmed directly by
# disassembling libLLVMSupport.a: its Z3Solver.cpp.o object references
# 126 undefined Z3_* symbols (LLVM's optional SMT-solver wrapper, used by
# nothing KAI-CC calls). This is NOT something `llvm_map_components_to_
# libnames`'s component selection can avoid - LLVMSupport is a mandatory
# transitive dependency of every component KAI-CC uses, and the Z3
# dependency is compiled into that one archive regardless of which other
# components are requested. Fedora's own LLVM 22 package was simply built
# without Z3 support, so this whole block is a no-op there (the `if`
# below never triggers).
#
# No static libz3.a is available via apt (Ubuntu's libz3-dev ships only
# libz3.so/libz3.so.4) and building Z3 from source is out of scope for
# this milestone - so the resolution is to bundle the EXACT SAME
# libz3.so.4 KAI's own build already links against, install it alongside
# libkai_runtime.a, and give the installed kaicc an origin-relative RPATH
# so it resolves ONLY that bundled copy at runtime (never a build-tree or
# system path). $ORIGIN is a literal linker/loader token, not a CMake
# variable - CMake passes it through unmodified into the installed
# binary's RPATH.
get_target_property(KAI_LLVM_SUPPORT_LIBS LLVMSupport INTERFACE_LINK_LIBRARIES)
set(KAI_Z3_LIBRARY "")
if(KAI_LLVM_SUPPORT_LIBS)
    foreach(_kai_llvm_support_lib ${KAI_LLVM_SUPPORT_LIBS})
        if(_kai_llvm_support_lib MATCHES "libz3\\.so")
            set(KAI_Z3_LIBRARY "${_kai_llvm_support_lib}")
        endif()
    endforeach()
endif()

if(KAI_Z3_LIBRARY)
    get_filename_component(KAI_Z3_LIBRARY_REALPATH "${KAI_Z3_LIBRARY}" REALPATH)
    message(STATUS "LLVM was built with Z3 support (${KAI_Z3_LIBRARY_REALPATH}) - "
                    "bundling it into the installed release layout")

    install(FILES "${KAI_Z3_LIBRARY_REALPATH}"
        DESTINATION lib/kai
    )

    # INSTALL_RPATH only affects the RPATH baked in by `cmake --install` -
    # it does NOT change build/bin/kaicc's own behavior (ordinary local
    # development/ctest are unaffected).
    set_target_properties(kaicc PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib/kai"
    )
endif()

# Tests
enable_testing()
add_subdirectory(tests)