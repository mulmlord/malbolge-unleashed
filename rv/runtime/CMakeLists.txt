# The picolibc runtime (issue #32, docs/rv.md 6) for `malm build --libc picolibc`, C++ (issue #33)
# and Lua (issue #36): picolibc, compiler-rt's builtins, picolibc/'s system interface, libc++ with
# libc++abi (and libunwind for exceptions) and Lua's interpreter (lua/), cross-compiled with the RISC-V toolchain (cmake/RvToolchain.cmake)
# into a sysroot (include/, lib/) that is packed into picolibc.tar.zst and built into malm
# (src/driver). The minimal runtime here (crt0, link.ld, include/, src/) needs no build step: malm
# embeds its sources (cmake/EmbedRuntime.cmake).
set(MALM_PICOLIBC AUTO CACHE STRING "Build picolibc, libc++ and Lua into malm (malm build --libc \
picolibc, C++, Lua): AUTO (if the RISC-V toolchain is found), ON, OFF")
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

# Sources only: they are built with riscv32.cmake below, not as part of this project.
include(FetchContent)
FetchContent_Declare(picolibc
    URL https://github.com/picolibc/picolibc/releases/download/1.8.12/picolibc-1.8.12.tar.xz
    URL_HASH SHA256=64e8c412e1c40fa6eb1a72d2b5cdbcbfe6ceca4cbea454edbad54557ffc747fa
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR none)
# Lua 5.5.1 for `malm build script.lua`, built into liblua.a below.
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR none)
# LLVM's whole source (140 MB), of which only compiler-rt's builtins and the C++ runtimes with what
# their build needs (runtimes/, cmake/, llvm/cmake and LLVM libc's headers, which libc++'s
# floating-point from_chars uses) are extracted, once.
FetchContent_Declare(llvm_project
    URL https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/llvm-project-20.1.8.src.tar.xz
    URL_HASH SHA256=6898f963c8e938981e6c4a302e83ec5beb4630147c7311183cf61069af16333d
    DOWNLOAD_NO_EXTRACT ON
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(picolibc lua llvm_project)
set(llvm_parts compiler-rt/lib/builtins libcxx libcxxabi libunwind runtimes cmake llvm/cmake
    libc/shared libc/src/__support libc/src/errno libc/hdr libc/include)
set(llvm_dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-project")
set(llvm_stamp "${llvm_dir}/extracted.txt")
if(EXISTS "${llvm_stamp}")
    file(READ "${llvm_stamp}" llvm_extracted)
endif()
if(NOT llvm_extracted STREQUAL "${llvm_parts}")
    file(REMOVE_RECURSE "${llvm_dir}")
    list(TRANSFORM llvm_parts PREPEND "llvm-project-20.1.8.src/" OUTPUT_VARIABLE patterns)
    message(STATUS "Extracting LLVM's compiler-rt builtins and C++ runtimes")
    file(ARCHIVE_EXTRACT INPUT "${llvm_project_SOURCE_DIR}/llvm-project-20.1.8.src.tar.xz"
        DESTINATION "${llvm_dir}" PATTERNS ${patterns})
    file(WRITE "${llvm_stamp}" "${llvm_parts}")
endif()
set(llvm_src "${llvm_dir}/llvm-project-20.1.8.src")

include(ExternalProject)
set(sysroot "${CMAKE_CURRENT_BINARY_DIR}/sysroot")
set(toolchain "${CMAKE_CURRENT_BINARY_DIR}/riscv32.cmake")
configure_file(picolibc/riscv32.cmake.in "${toolchain}" @ONLY)
set(cross_args -DCMAKE_TOOLCHAIN_FILE=${toolchain} -DCMAKE_INSTALL_PREFIX=${sysroot}
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE=-O2)
set(libs lib/libc.a lib/libmalm.a lib/libclang_rt.builtins.a lib/libc++.a lib/libc++abi.a
    lib/eh/libc++.a lib/eh/libc++abi.a lib/eh/libunwind.a lib/liblua.a)

# One hart and no thread pointer: errno is a plain global, stdio takes no locks.
ExternalProject_Add(picolibc_libc
    SOURCE_DIR "${picolibc_SOURCE_DIR}"
    CMAKE_ARGS ${cross_args} -D__THREAD_LOCAL_STORAGE=OFF -D__SINGLE_THREAD=ON
    BUILD_BYPRODUCTS "${sysroot}/lib/libc.a")
# Built every time, as ExternalProject does not see changes to its sources; a no-op if none.
ExternalProject_Add(picolibc_runtime
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/picolibc"
    CMAKE_ARGS ${cross_args} -DSYSROOT=${sysroot} -DCOMPILER_RT_DIR=${llvm_src}/compiler-rt
        -DWERROR=${MALM_WERROR}
    DEPENDS picolibc_libc
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${sysroot}/lib/libmalm.a" "${sysroot}/lib/libclang_rt.builtins.a")
# Lua and its interpreter's main, built every time like the runtime above.
ExternalProject_Add(picolibc_lua
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lua"
    CMAKE_ARGS ${cross_args} -DSYSROOT=${sysroot} -DLUA_DIR=${lua_SOURCE_DIR}
        -DWERROR=${MALM_WERROR}
    DEPENDS picolibc_libc
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${sysroot}/lib/liblua.a")

# libc++ and libc++abi for C++ (docs/rv.md 6), twice: in lib/ without exceptions (the default), in
# lib/eh/ with them and libunwind (`-fexceptions`). One hart, no files, clock or locales (so no
# iostreams), the "C" character set only. -fbinutils-version=none lets ld.lld drop a function's
# exception table with the function. The uncaught-exception message names the type mangled, which
# saves linking the demangler (110 KB).
set(cxx_flags "-march=rv32im -mabi=ilp32 -nostdlibinc -isystem ${sysroot}/include \
-ffunction-sections -fdata-sections -fbinutils-version=none")
set(libcxx_args ${cross_args} "-DCMAKE_C_FLAGS=${cxx_flags}" "-DCMAKE_CXX_FLAGS=${cxx_flags}"
    -DCMAKE_CXX_FLAGS_RELEASE=-O2 -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_THREADS=OFF -DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF
    -DLIBCXX_ENABLE_FILESYSTEM=OFF -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF
    -DLIBCXX_ENABLE_LOCALIZATION=OFF -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF
    -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_INSTALL_MODULES=OFF
    -DLIBCXXABI_BAREMETAL=ON -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_THREADS=OFF
    -DLIBCXXABI_ENABLE_ASSERTIONS=OFF -DLIBCXXABI_USE_COMPILER_RT=ON
    -DLIBCXXABI_NON_DEMANGLING_TERMINATE=ON
    # Their shared libraries are built as static ones for a target without them; other names
    # keep them apart (as in libc++'s Armv7M-picolibc.cmake).
    -DLIBCXX_SHARED_OUTPUT_NAME=c++-shared -DLIBCXXABI_SHARED_OUTPUT_NAME=c++abi-shared
    -DLIBUNWIND_SHARED_OUTPUT_NAME=unwind-shared)
# The headers are the same for both; libc++'s frozen C++03 headers (9 MB) only for those who ask.
ExternalProject_Add(picolibc_libcxx
    SOURCE_DIR "${llvm_src}/runtimes"
    LIST_SEPARATOR |
    CMAKE_ARGS ${libcxx_args} -DLLVM_ENABLE_RUNTIMES=libcxx|libcxxabi
        -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF
        -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
    DEPENDS picolibc_libc
    BUILD_BYPRODUCTS "${sysroot}/lib/libc++.a" "${sysroot}/lib/libc++abi.a")
ExternalProject_Add_Step(picolibc_libcxx prune
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${sysroot}/include/c++/v1/__cxx03"
    DEPENDEES install)
# Installed elsewhere (both builds run at once), then its libraries copied to lib/eh.
set(eh_prefix "${CMAKE_CURRENT_BINARY_DIR}/libcxx_eh")
ExternalProject_Add(picolibc_libcxx_eh
    SOURCE_DIR "${llvm_src}/runtimes"
    LIST_SEPARATOR |
    CMAKE_ARGS ${libcxx_args} -DCMAKE_INSTALL_PREFIX=${eh_prefix}
        -DLLVM_ENABLE_RUNTIMES=libcxx|libcxxabi|libunwind
        -DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=ON
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON
        -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_THREADS=OFF -DLIBUNWIND_IS_BAREMETAL=ON
        -DLIBUNWIND_USE_COMPILER_RT=ON
    DEPENDS picolibc_libc)
set(eh_libs "${eh_prefix}/lib/libc++.a" "${eh_prefix}/lib/libc++abi.a"
    "${eh_prefix}/lib/libunwind.a")
ExternalProject_Add_Step(picolibc_libcxx_eh copy
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${sysroot}/lib/eh"
    COMMAND "${CMAKE_COMMAND}" -E copy ${eh_libs} "${sysroot}/lib/eh"
    DEPENDEES install
    BYPRODUCTS "${sysroot}/lib/eh/libc++.a" "${sysroot}/lib/eh/libc++abi.a"
        "${sysroot}/lib/eh/libunwind.a")

set(archive "${CMAKE_CURRENT_BINARY_DIR}/picolibc.tar.zst")
list(TRANSFORM libs PREPEND "${sysroot}/" OUTPUT_VARIABLE libs_abs)
string(JOIN "|" paths include ${libs})
add_custom_command(OUTPUT "${archive}"
    COMMAND "${CMAKE_COMMAND}" -DARCHIVE=${archive} -DPATHS=${paths}
        -P "${PROJECT_SOURCE_DIR}/cmake/PackSysroot.cmake"
    DEPENDS picolibc_runtime picolibc_lua picolibc_libcxx picolibc_libcxx_eh ${libs_abs}
        "${PROJECT_SOURCE_DIR}/cmake/PackSysroot.cmake"
    WORKING_DIRECTORY "${sysroot}"
    COMMENT "Packing the picolibc sysroot"
    VERBATIM)
add_custom_target(picolibc_sysroot DEPENDS "${archive}")
set_target_properties(picolibc_sysroot PROPERTIES
    MALM_ARCHIVE "${archive}" MALM_SYSROOT "${sysroot}")
