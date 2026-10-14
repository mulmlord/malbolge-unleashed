# The RISC-V loader and reference emulator (issue #23), runtime (issue #24) and `malm build`
# (issue #26), docs/rv.md. Unit tests need no RISC-V toolchain; the riscv-tests suite and the C test
# programs are built with clang (riscv32 target) and ld.lld when they are found (MALM_RV_TESTS).
malm_add_test(NAME rv_test LABEL fast SOURCES elf_test.cpp machine_test.cpp rvref_test.cpp
    build_test.cpp sysroot_test.cpp LIBS malm_rv malm_driver)
# rv-emu (issue #25): single instructions against the reference emulator. A debug build spends
# about 3 minutes compiling the emulator, so there its cases are slow.
malm_add_test(NAME rv_emu_test LABEL fast SOURCES emu_test.cpp LIBS malm_rv malm_hla TIMEOUT 1800)
# rv-aot (issue #29): the control-flow graph, and translated programs against the reference
# emulator. A debug build's programs are slow.
malm_add_test(NAME rv_aot_test LABEL fast SOURCES aot_test.cpp LIBS malm_aot malm_hla TIMEOUT 1800)
# emu.malm as built (rv/emu): with zero RAM, the instruction at pc 0 is illegal, so it halts with 1.
# With 1M of guest RAM its startup takes 220 M steps and about 5 GB of VM memory.
if(TARGET emu)
    add_test(NAME emu_malm_halts
        COMMAND sh -c "\"$1\" run \"$2\" > /dev/null; test $? = 1" sh $<TARGET_FILE:malm>
            "${PROJECT_BINARY_DIR}/rv/emu/emu.malm")
    set_tests_properties(emu_malm_halts PROPERTIES LABELS fast TIMEOUT 600)
endif()

set(MALM_RV_TESTS AUTO CACHE STRING
    "Build and run riscv-tests and the RISC-V test programs: AUTO (if the toolchain is found), ON, OFF")
set_property(CACHE MALM_RV_TESTS PROPERTY STRINGS AUTO ON OFF)
if(MALM_RV_TESTS STREQUAL "OFF")
    return()
endif()

set(rv_flags ${MALM_RV_FLAGS})
if(NOT MALM_RV_TOOLCHAIN_OK)
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

# picolibc (issue #32), as built into malm (rv/runtime/CMakeLists.txt).
if(TARGET picolibc_sysroot)
    get_target_property(pl_sysroot picolibc_sysroot MALM_SYSROOT)
    set(pl_libs "${pl_sysroot}/lib/libmalm.a" "${pl_sysroot}/lib/libc.a"
        "${pl_sysroot}/lib/libclang_rt.builtins.a")
endif()

# rv_elf(<name> <source> [RUNTIME | PICOLIBC] [DEPENDS <files>...] [FLAGS <flags>...]
#        [LDFLAGS <flags>...]): compile and link one test ELF; RUNTIME builds it against the
# runtime's headers and links crt0 and the libc, PICOLIBC the same with picolibc.
function(rv_elf name src)
    cmake_parse_arguments(ARG "RUNTIME;PICOLIBC" "" "DEPENDS;FLAGS;LDFLAGS" ${ARGN})
    set(obj "${rv_elf_dir}/${name}.o")
    set(elf "${rv_elf_dir}/${name}.elf")
    set(inputs "${obj}")
    set(ld_flags ${ARG_LDFLAGS})
    if(ARG_RUNTIME)
        list(PREPEND ARG_FLAGS ${rt_cflags})
        list(APPEND ARG_DEPENDS ${rt_headers} "${rt_crt0}" ${rt_libc})
        set(inputs "${rt_crt0}" "${obj}" --start-lib ${rt_libc} --end-lib)
        list(APPEND ld_flags --gc-sections)
    elseif(ARG_PICOLIBC)
        list(PREPEND ARG_FLAGS -O2 -nostdlibinc -isystem "${pl_sysroot}/include" -ffunction-sections
            -fdata-sections)
        list(APPEND ARG_DEPENDS picolibc_sysroot "${rt_crt0}" ${pl_libs})
        set(inputs "${rt_crt0}" "${obj}" ${pl_libs})
        list(APPEND ld_flags --gc-sections)
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

# malm build's C toolchain (issue #28): the runtime built into malm, compiled with this clang.
malm_add_test(NAME rv_toolchain_test LABEL fast SOURCES toolchain_test.cpp
    LIBS malm_rv malm_driver TIMEOUT 300)
target_compile_definitions(rv_toolchain_test PRIVATE MALM_RV_CC="${MALM_RV_CC}"
    MALM_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/c")

# Programs on the runtime: the C examples and the runtime's self-check.
foreach(prog hello cat fizzbuzz wc sort)
    rv_elf(example_${prog} "${PROJECT_SOURCE_DIR}/examples/c/${prog}.c" RUNTIME
        FLAGS -Wall -Wextra -Werror)
endforeach()
rv_elf(runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/runtime_test.c" RUNTIME
    FLAGS -Wall -Wextra -Werror)

# The same programs dumping their registers as they exit (ecall 255), for the three-way
# differential test: the bare ones with -DMALM_DUMP_REGS, the others linked with
# __malm_dump_regs (and a 16K heap reserve, so that sort has a heap in 64K of guest RAM).
foreach(prog hello cat)
    rv_elf(dump_${prog} "${CMAKE_CURRENT_SOURCE_DIR}/programs/${prog}.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/programs/sys.h"
        FLAGS -O2 -ffreestanding -Wall -Wextra -Werror -DMALM_DUMP_REGS)
endforeach()
foreach(prog hello cat fizzbuzz wc sort)
    rv_elf(dump_example_${prog} "${PROJECT_SOURCE_DIR}/examples/c/${prog}.c" RUNTIME
        FLAGS -Wall -Wextra -Werror
        LDFLAGS --defsym=__malm_dump_regs=1 --defsym=__stack_size=16384)
endforeach()
rv_elf(dump_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/runtime_test.c" RUNTIME
    FLAGS -Wall -Wextra -Werror LDFLAGS --defsym=__malm_dump_regs=1)

# The same with picolibc, and the example that needs it.
set(rv_picolibc_cases "")
if(TARGET picolibc_sysroot)
    foreach(prog hello cat fizzbuzz wc sort stats)
        rv_elf(picolibc_example_${prog} "${PROJECT_SOURCE_DIR}/examples/c/${prog}.c" PICOLIBC
            FLAGS -Wall -Wextra -Werror)
    endforeach()
    rv_elf(picolibc_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/runtime_test.c" PICOLIBC
        FLAGS -Wall -Wextra -Werror)
    set(rv_picolibc_cases example_hello example_cat fizzbuzz wc sort stats runtime runtime_assert)
endif()

add_custom_target(rv_test_elfs ALL DEPENDS ${rv_elfs})

foreach(name IN LISTS rv_suite_tests)
    add_test(NAME rvref_${name} COMMAND $<TARGET_FILE:malm> rvref "${rv_elf_dir}/${name}.elf")
    set_tests_properties(rvref_${name} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()

foreach(case hello cat trace regs example_hello example_cat fizzbuzz wc sort runtime runtime_assert)
    add_test(NAME rvref_program_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rvref_pipe_test.sh" $<TARGET_FILE:malm>
            "${rv_elf_dir}" "${PROJECT_SOURCE_DIR}/tests/programs" ${case})
    set_tests_properties(rvref_program_${case} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()
foreach(case IN LISTS rv_picolibc_cases)
    add_test(NAME rvref_picolibc_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rvref_pipe_test.sh" $<TARGET_FILE:malm>
            "${rv_elf_dir}" "${PROJECT_SOURCE_DIR}/tests/programs" ${case} picolibc)
    set_tests_properties(rvref_picolibc_${case} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()

# The three-way differential test (issue #30): each program on the reference emulator and built
# in emu and aot mode (`malm build`, `malm run`), through real pipes; stdout, which ends with the
# register dump, and the exit status must agree. Only where emu.malm is built (not in the debug
# preset, which would need about 4 minutes per build). hello is fast, the rest slow (nightly); the
# runtime's self-check runs 3 M guest instructions, most of an hour in emu mode.
set(rv_inputs "${CMAKE_CURRENT_SOURCE_DIR}/inputs")
if(NOT MALM_BUILD_EMU)
    return()
endif()
foreach(spec
        "hello|dump_hello|64K|/dev/null"
        "cat|dump_cat|64K|${PROJECT_SOURCE_DIR}/tests/programs/cat5_input.bin|/dev/null"
        "example_hello|dump_example_hello|64K|/dev/null"
        "example_cat|dump_example_cat|64K|${rv_inputs}/hi.in|${PROJECT_SOURCE_DIR}/tests/programs/cat5_input.bin|/dev/null"
        "wc|dump_example_wc|64K|${rv_inputs}/wc.in"
        "fizzbuzz|dump_example_fizzbuzz|64K|${rv_inputs}/fizzbuzz.in|${rv_inputs}/fizzbuzz_bad.in"
        "sort|dump_example_sort|64K|${rv_inputs}/sort.in"
        "runtime|dump_runtime_test|256K|${rv_inputs}/runtime.in|${rv_inputs}/runtime_assert.in")
    string(REPLACE "|" ";" fields "${spec}")
    list(POP_FRONT fields case elf ram)
    set(label slow)
    if(case STREQUAL "example_hello")
        set(label fast)
    endif()
    add_test(NAME rvdiff_program_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rv_diff_test.sh" $<TARGET_FILE:malm>
            "${rv_elf_dir}/${elf}.elf" ${ram} ${fields})
    set_tests_properties(rvdiff_program_${case} PROPERTIES LABELS ${label} TIMEOUT 7200)
endforeach()

# `malm build` for C (issue #28): each example built into a .malm and a native executable and run
# through pipes; cat is the M3 exit criterion (`echo hi | ./cat`) and fast, the rest slow.
foreach(case hello cat wc fizzbuzz sort)
    set(label slow)
    if(case STREQUAL "cat")
        set(label fast)
    endif()
    add_test(NAME build_c_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
            "${MALM_RV_CC}" "${PROJECT_SOURCE_DIR}/examples/c" "${PROJECT_SOURCE_DIR}/tests/programs"
            ${case})
    set_tests_properties(build_c_${case} PROPERTIES LABELS ${label} TIMEOUT 1800)
    # In aot mode (issue #29): the same output as in emu mode.
    add_test(NAME build_c_aot_${case}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
            "${MALM_RV_CC}" "${PROJECT_SOURCE_DIR}/examples/c" "${PROJECT_SOURCE_DIR}/tests/programs"
            ${case} aot)
    set_tests_properties(build_c_aot_${case} PROPERTIES LABELS ${label} TIMEOUT 1800)
endforeach()
# With picolibc (issue #32), in emu mode; stats needs it and is fast.
if(TARGET picolibc_sysroot)
    foreach(case hello cat wc fizzbuzz sort stats)
        set(label slow)
        if(case STREQUAL "stats")
            set(label fast)
        endif()
        add_test(NAME build_c_picolibc_${case}
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
                "${MALM_RV_CC}" "${PROJECT_SOURCE_DIR}/examples/c"
                "${PROJECT_SOURCE_DIR}/tests/programs" ${case} emu picolibc)
        set_tests_properties(build_c_picolibc_${case} PROPERTIES LABELS ${label} TIMEOUT 1800)
    endforeach()
endif()

# riscv-tests (issues #27, #29, #30): each test on the reference emulator and built in emu and aot
# mode (8K of guest RAM: every test ends below 0x2000 and uses no stack), with the three-way
# differential test; the reference emulator must also pass it (status 0). A subset is fast, the
# rest slow (nightly). fence_i modifies its code: aot mode must refuse it.
set(rv_diff_fast_tests rv32ui-p-add rv32ui-p-jalr rv32ui-p-lw rv32ui-p-ma_data rv32ui-p-fence_i)
foreach(name IN LISTS rv_suite_tests)
    set(label slow)
    if(name IN_LIST rv_diff_fast_tests)
        set(label fast)
    endif()
    set(smc "")
    if(name STREQUAL "rv32ui-p-fence_i")
        set(smc --aot-smc)
    endif()
    add_test(NAME rvdiff_${name}
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rv_diff_test.sh" $<TARGET_FILE:malm>
            "${rv_elf_dir}/${name}.elf" 8K --status 0 ${smc} -- /dev/null)
    set_tests_properties(rvdiff_${name} PROPERTIES LABELS ${label} TIMEOUT 600)
endforeach()
