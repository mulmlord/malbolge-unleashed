cmake_minimum_required(VERSION 3.28)
project(malbolge-unleashed VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules; scanning would require clang-scan-deps with Clang.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(MALM_WERROR "Treat warnings as errors" OFF)
option(MALM_SANITIZE "Build with ASan + UBSan" OFF)
option(MALM_STATIC "Link malm statically" OFF)
option(MALM_BUILD_TESTS "Build the test suite" ON)

if(MALM_STATIC AND MALM_SANITIZE)
    message(FATAL_ERROR "MALM_STATIC and MALM_SANITIZE cannot be combined (ASan does not support -static)")
endif()

# Flags for our own targets only (never applied globally, so third-party code is untouched).
add_library(malm_warnings INTERFACE)
target_compile_options(malm_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(MALM_WERROR)
    target_compile_options(malm_warnings INTERFACE -Werror)
endif()
if(MALM_SANITIZE)
    target_compile_options(malm_warnings INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(malm_warnings INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

add_subdirectory(src/core)
add_subdirectory(src/asm)
add_subdirectory(src/hla)
add_subdirectory(src/container)
add_subdirectory(src/pack)
add_subdirectory(src/driver)

if(MALM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
