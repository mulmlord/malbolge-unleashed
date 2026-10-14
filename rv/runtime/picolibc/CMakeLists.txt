# The RISC-V side of the picolibc runtime (docs/rv.md 6), cross-compiled by
# rv/runtime/CMakeLists.txt with riscv32.cmake into the sysroot that already holds picolibc:
# libmalm.a, picolibc's system interface (the system calls, the standard streams and the rest of
# POSIX it needs), and libclang_rt.builtins.a, compiler-rt's soft-float and 64-bit integer routines.
cmake_minimum_required(VERSION 3.28)
project(malm_picolibc_runtime LANGUAGES C)

set(SYSROOT "" CACHE PATH "The sysroot: picolibc installed there, and these libraries")
set(COMPILER_RT_DIR "" CACHE PATH "compiler-rt's source")
option(WERROR "Treat warnings as errors" OFF)

set(common -nostdlibinc -isystem "${SYSROOT}/include" -ffreestanding -ffunction-sections
    -fdata-sections)

add_library(malm STATIC posix.c stdio.c ../src/syscalls.c)
target_compile_options(malm PRIVATE ${common} -std=c11
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion $<$<BOOL:${WERROR}>:-Werror>)

# lib/builtins' GENERIC_SOURCES for riscv32, without the Apple-only files and the 128-bit ones (ti),
# which build to nothing as riscv32 has no __int128. So do GENERIC_TF_SOURCES (long double, which
# is 128-bit on riscv32), left out too: long double arithmetic does not link.
set(builtins
    absvdi2 absvsi2 adddf3 addsf3 addvdi3 addvsi3 ashldi3 ashrdi3 bswapdi2 bswapsi2 clzdi2 clzsi2
    cmpdi2 comparedf2 comparesf2 ctzdi2 ctzsi2 divdc3 divdf3 divdi3 divmoddi4 divmodsi4 divsc3
    divsf3 divsi3 extendhfsf2 extendsfdf2 ffsdi2 ffssi2 fixdfdi fixdfsi fixsfdi fixsfsi fixunsdfdi
    fixunsdfsi fixunssfdi fixunssfsi floatdidf floatdisf floatsidf floatsisf floatundidf floatundisf
    floatunsidf floatunsisf fp_mode int_util lshrdi3 moddi3 modsi3 muldc3 muldf3 muldi3 mulodi4
    mulosi4 mulsc3 mulsf3 mulvdi3 mulvsi3 negdf2 negdi2 negsf2 negvdi2 negvsi2 paritydi2 paritysi2
    popcountdi2 popcountsi2 powidf2 powisf2 subdf3 subsf3 subvdi3 subvsi3 truncdfhf2 truncdfsf2
    truncsfhf2 ucmpdi2 udivdi3 udivmoddi4 udivmodsi4 udivsi3 umoddi3 umodsi3)
list(TRANSFORM builtins PREPEND "${COMPILER_RT_DIR}/lib/builtins/")
list(TRANSFORM builtins APPEND ".c")
add_library(clang_rt.builtins STATIC ${builtins})
target_compile_options(clang_rt.builtins PRIVATE ${common} -fno-builtin -std=c11)

install(TARGETS malm clang_rt.builtins ARCHIVE DESTINATION lib)
