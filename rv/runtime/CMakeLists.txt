# The picolibc runtime (issue #32, docs/rv.md 6) for `malm build --libc picolibc`: picolibc,
# compiler-rt's builtins and picolibc/'s system interface, cross-compiled with the RISC-V toolchain
# (cmake/RvToolchain.cmake) into a sysroot (include/, lib/) that is packed into picolibc.tar.zst and
# built into malm (src/driver). The minimal runtime here (crt0, link.ld, include/, src/) needs no
# build step: malm embeds its sources (cmake/EmbedRuntime.cmake).
set(MALM_PICOLIBC AUTO CACHE STRING "Build picolibc into malm (malm build --libc picolibc): \
AUTO (if the RISC-V toolchain is found), ON, OFF")
set_property(CACHE MALM_PICOLIBC PROPERTY STRINGS AUTO ON OFF)
if(MALM_PICOLIBC STREQUAL "OFF")
    return()
endif()
if(NOT MALM_RV_TOOLCHAIN_OK OR NOT MALM_RV_AR)
    set(msg "picolibc needs clang with the riscv32 target, ld.lld and llvm-ar or ar \
(found: MALM_RV_CC=${MALM_RV_CC}, MALM_RV_LD=${MALM_RV_LD}, MALM_RV_AR=${MALM_RV_AR})")
    if(MALM_PICOLIBC STREQUAL "ON")
        message(FATAL_ERROR "${msg}")
    endif()
    message(WARNING "${msg}; building malm without it (set MALM_PICOLIBC=OFF to silence this)")
    return()
endif()

# Sources only: both are built with riscv32.cmake below, not as part of this project.
include(FetchContent)
FetchContent_Declare(picolibc
    URL https://github.com/picolibc/picolibc/releases/download/1.8.12/picolibc-1.8.12.tar.xz
    URL_HASH SHA256=64e8c412e1c40fa6eb1a72d2b5cdbcbfe6ceca4cbea454edbad54557ffc747fa
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR none)
FetchContent_Declare(compiler_rt
    URL https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/compiler-rt-20.1.8.src.tar.xz
    URL_HASH SHA256=15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(picolibc compiler_rt)

include(ExternalProject)
set(sysroot "${CMAKE_CURRENT_BINARY_DIR}/sysroot")
set(toolchain "${CMAKE_CURRENT_BINARY_DIR}/riscv32.cmake")
configure_file(picolibc/riscv32.cmake.in "${toolchain}" @ONLY)
set(cross_args -DCMAKE_TOOLCHAIN_FILE=${toolchain} -DCMAKE_INSTALL_PREFIX=${sysroot}
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE=-O2)
set(libs lib/libc.a lib/libmalm.a lib/libclang_rt.builtins.a)

# One hart and no thread pointer: errno is a plain global, stdio takes no locks.
ExternalProject_Add(picolibc_libc
    SOURCE_DIR "${picolibc_SOURCE_DIR}"
    CMAKE_ARGS ${cross_args} -D__THREAD_LOCAL_STORAGE=OFF -D__SINGLE_THREAD=ON
    BUILD_BYPRODUCTS "${sysroot}/lib/libc.a")
# Built every time, as ExternalProject does not see changes to its sources; a no-op if none.
ExternalProject_Add(picolibc_runtime
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/picolibc"
    CMAKE_ARGS ${cross_args} -DSYSROOT=${sysroot} -DCOMPILER_RT_DIR=${compiler_rt_SOURCE_DIR}
        -DWERROR=${MALM_WERROR}
    DEPENDS picolibc_libc
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${sysroot}/lib/libmalm.a" "${sysroot}/lib/libclang_rt.builtins.a")

set(archive "${CMAKE_CURRENT_BINARY_DIR}/picolibc.tar.zst")
list(TRANSFORM libs PREPEND "${sysroot}/" OUTPUT_VARIABLE libs_abs)
add_custom_command(OUTPUT "${archive}"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --zstd "--mtime=1970-01-01 00:00:00 UTC"
        include ${libs}
    DEPENDS picolibc_runtime ${libs_abs}
    WORKING_DIRECTORY "${sysroot}"
    COMMENT "Packing the picolibc sysroot"
    VERBATIM)
add_custom_target(picolibc_sysroot DEPENDS "${archive}")
set_target_properties(picolibc_sysroot PROPERTIES
    MALM_ARCHIVE "${archive}" MALM_SYSROOT "${sysroot}")
