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

# Main compiler executable
add_executable(kaicc
    compiler/src/main.cpp
)

# Public compiler headers
target_include_directories(kaicc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/compiler/include
)

# Compiler warnings
if(MSVC)
    target_compile_options(kaicc PRIVATE
        /W4
        /permissive-
    )
else()
    target_compile_options(kaicc PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()