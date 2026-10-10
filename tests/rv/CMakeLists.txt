# The RISC-V loader and reference emulator (issue #23) and runtime (issue #24), docs/rv.md. Unit
# tests need no RISC-V toolchain; the riscv-tests suite and the C test programs are built with clang
# (riscv32 target) and ld.lld when they are found (MALM_RV_TESTS).
malm_add_test(NAME rv_test LABEL fast SOURCES elf_test.cpp machine_test.cpp rvref_test.cpp
    LIBS malm_rv malm_driver)

set(MALM_RV_TESTS AUTO CACHE STRING
    "Build and run riscv-tests and the RISC-V test programs: AUTO (if the toolchain is found), ON, OFF")
set_property(CACHE MALM_RV_TESTS PROPERTY STRINGS AUTO ON OFF)
if(MALM_RV_TESTS STREQUAL "OFF")
    return()
endif()

# clang with the riscv32 target compiles, ld.lld links (called directly, so no driver linker
# search is involved). Versioned names cover Ubuntu's clang-18 / lld-18.
find_program(MALM_RV_CC NAMES clang clang-22 clang-21 clang-20 clang-19 clang-18)
if(MALM_RV_CC)
    file(REAL_PATH "${MALM_RV_CC}" rv_cc_real)
    cmake_path(GET rv_cc_real PARENT_PATH rv_cc_dir)
endif()
find_program(MALM_RV_LD NAMES ld.lld ld.lld-22 ld.lld-21 ld.lld-20 ld.lld-19 ld.lld-18
    HINTS "${rv_cc_dir}")
set(rv_flags --target=riscv32-unknown-elf -march=rv32im_zifencei -mabi=ilp32)
set(rv_ok FALSE)
if(MALM_RV_CC AND MALM_RV_LD)
    set(probe "${CMAKE_CURRENT_BINARY_DIR}/probe")
    file(WRITE "${probe}.S" "_start: ecall\n")
    execute_process(COMMAND "${MALM_RV_CC}" ${rv_flags} -c "${probe}.S" -o "${probe}.o"
        RESULT_VARIABLE cc_rc OUTPUT_QUIET ERROR_QUIET)
    if(cc_rc EQUAL 0)
        execute_process(COMMAND "${MALM_RV_LD}" "${probe}.o" -o "${probe}.elf"
            RESULT_VARIABLE ld_rc OUTPUT_QUIET ERROR_QUIET)
        if(ld_rc EQUAL 0)
            set(rv_ok TRUE)
        endif()
    endif()
endif()
if(NOT rv_ok)
    set(msg "RISC-V tests need clang with the riscv32 target and ld.lld \
(found: MALM_RV_CC=${MALM_RV_CC}, MALM_RV_LD=${MALM_RV_LD})")
    if(MALM_RV_TESTS STREQUAL "ON")
        message(FATAL_ERROR "${msg}")
    endif()
    message(WARNING "${msg}; skipping them (set MALM_RV_TESTS=OFF to silence this)")
    return()
endif()

include(FetchContent)
FetchContent_Declare(riscv_tests
    URL https://github.com/riscv-software-src/riscv-tests/archive/7b68f7b91f827f8d94cc1c57ff123752c5c8f704.tar.gz
    URL_HASH SHA256=c6fa241a06b756f73df66c2a23dd3fb25d2177a6744fa9cc523cff26fd538aae
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(riscv_tests)

set(rt_dir "${PROJECT_SOURCE_DIR}/rv/runtime")
set(rv_ld_script "${rt_dir}/link.ld")
set(rv_elf_dir "${CMAKE_CURRENT_BINARY_DIR}/elf")
file(MAKE_DIRECTORY "${rv_elf_dir}/runtime")
set(rv_elfs "")

# The runtime: crt0 and one object per libc source, linked as a lazy library (--start-lib) so that
# a program pulls in only the members it uses, and with --gc-sections only the functions.
file(GLOB rt_headers CONFIGURE_DEPENDS "${rt_dir}/include/*.h")
file(GLOB rt_sources CONFIGURE_DEPENDS "${rt_dir}/src/*.c")
set(rt_cflags -O2 -nostdlibinc -isystem "${rt_dir}/include" -ffunction-sections -fdata-sections)
set(rt_crt0 "${rv_elf_dir}/runtime/crt0.o")
add_custom_command(OUTPUT "${rt_crt0}"
    COMMAND "${MALM_RV_CC}" ${rv_flags} -c "${rt_dir}/crt0.S" -o "${rt_crt0}"
    DEPENDS "${rt_dir}/crt0.S"
    COMMENT "Building RISC-V runtime crt0"
    VERBATIM)
set(rt_libc "")
foreach(src IN LISTS rt_sources)
    cmake_path(GET src STEM stem)
    set(obj "${rv_elf_dir}/runtime/${stem}.o")
    add_custom_command(OUTPUT "${obj}"
        COMMAND "${MALM_RV_CC}" ${rv_flags} ${rt_cflags} -ffreestanding -std=c11
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror
            -c "${src}" -o "${obj}"
        DEPENDS "${src}" ${rt_headers}
        COMMENT "Building RISC-V runtime ${stem}"
        VERBATIM)
    list(APPEND rt_libc "${obj}")
endforeach()

# rv_elf(<name> <source> [RUNTIME] [DEPENDS <files>...] [FLAGS <flags>...]): compile and link one
# test ELF; RUNTIME builds it against the runtime's headers and links crt0 and the libc.
function(rv_elf name src)
    cmake_parse_arguments(ARG "RUNTIME" "" "DEPENDS;FLAGS" ${ARGN})
    set(obj "${rv_elf_dir}/${name}.o")
    set(elf "${rv_elf_dir}/${name}.elf")
    set(inputs "${obj}")
    set(ld_flags "")
    if(ARG_RUNTIME)
        list(PREPEND ARG_FLAGS ${rt_cflags})
        list(APPEND ARG_DEPENDS ${rt_headers} "${rt_crt0}" ${rt_libc})
        set(inputs "${rt_crt0}" "${obj}" --start-lib ${rt_libc} --end-lib)
        set(ld_flags --gc-sections)
    endif()
    add_custom_command(OUTPUT "${elf}"
        COMMAND "${MALM_RV_CC}" ${rv_flags} ${ARG_FLAGS} -c "${src}" -o "${obj}"
        COMMAND "${MALM_RV_LD}" -T "${rv_ld_script}" ${ld_flags} ${inputs} -o "${elf}"
        DEPENDS "${src}" "${rv_ld_script}" ${ARG_DEPENDS}
        COMMENT "Building RISC-V test ${name}"
        VERBATIM)
    set(rv_elfs ${rv_elfs} "${elf}" PARENT_SCOPE)
endfunction()

# riscv-tests rv32ui-p-* and rv32um-p-*, against our environment (env/riscv_test.h): each test
# exits with status 0 on pass, the failing test number otherwise.
set(rv_isa "${riscv_tests_SOURCE_DIR}/isa")
set(rv_suite_tests "")
foreach(suite rv32ui rv32um)
    file(GLOB srcs CONFIGURE_DEPENDS "${rv_isa}/${suite}/*.S")
    foreach(src IN LISTS srcs)
        cmake_path(GET src STEM test)
        set(name "${suite}-p-${test}")
        rv_elf(${name} "${src}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/env/riscv_test.h"
            FLAGS -I "${CMAKE_CURRENT_SOURCE_DIR}/env" -I "${rv_isa}/macros/scalar")
        list(APPEND rv_suite_tests ${name})
    endforeach()
endforeach()

# Bare C programs without the runtime (sys.h), for the emulator's initial and final state.
foreach(prog hello cat)
    rv_elf(${prog} "${CMAKE_CURRENT_SOURCE_DIR}/programs/${prog}.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/programs/sys.h"
        FLAGS -O2 -ffreestanding -Wall -Wextra -Werror)
endforeach()

# Programs on the runtime: the C examples and the runtime's self-check.
foreach(prog hello cat fizzbuzz)
    rv_elf(example_${prog} "${PROJECT_SOURCE_DIR}/examples/c/${prog}.c" RUNTIME
        FLAGS -Wall -Wextra -Werror)
endforeach()
rv_elf(runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/runtime_test.c" RUNTIME
    FLAGS -Wall -Wextra -Werror)

add_custom_target(rv_test_elfs ALL DEPENDS ${rv_elfs})

foreach(name IN LISTS rv_suite_tests)
    add_test(NAME rvref_${name} COMMAND $<TARGET_FILE:malm> rvref "${rv_elf_dir}/${name}.elf")
    set_tests_properties(rvref_${name} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()

foreach(case hello cat trace regs example_hello example_cat fizzbuzz runtime runtime_assert)
    add_test(NAME rvref_program_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rvref_pipe_test.sh" $<TARGET_FILE:malm>
            "${rv_elf_dir}" "${PROJECT_SOURCE_DIR}/tests/programs" ${case})
    set_tests_properties(rvref_program_${case} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()
