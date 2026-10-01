# zstd (pinned). Its own CMake project is used as-is: our warning/sanitizer flags live on the
# malm_warnings interface target and reach only our targets, never zstd.
include(FetchContent)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR build/cmake
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(zstd)

add_library(malm_container STATIC container.cpp)
target_include_directories(malm_container PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_container PUBLIC malm_core PRIVATE malm_warnings libzstd_static)
# zstd.h for the tests, which forge frames.
target_include_directories(malm_container SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
