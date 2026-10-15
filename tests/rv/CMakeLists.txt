# The RISC-V loader and reference emulator (issue #23), runtime (issue #24) and `malm build`
# (issue #26), docs/rv.md. Unit tests need no RISC-V toolchain; the riscv-tests suite and the C test
# programs are built with clang (riscv32 target) and ld.lld when they are found (MALM_RV_TESTS), the
# Rust ones with rustc and its riscv32im target (MALM_RUST_TESTS), the Zig ones with zig 0.17
# (MALM_ZIG_TESTS).
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

# picolibc (issue #32) and libc++ (issue #33), as built into malm (rv/runtime/CMakeLists.txt).
if(TARGET picolibc_sysroot)
    get_target_property(pl_sysroot picolibc_sysroot MALM_SYSROOT)
    set(pl_libs "${pl_sysroot}/lib/libmalm.a" "${pl_sysroot}/lib/libc.a"
        "${pl_sysroot}/lib/libclang_rt.builtins.a")
    set(cxx_libs "${pl_sysroot}/lib/libc++.a" "${pl_sysroot}/lib/libc++abi.a")
    set(cxx_eh_libs "${pl_sysroot}/lib/eh/libc++.a" "${pl_sysroot}/lib/eh/libc++abi.a"
        "${pl_sysroot}/lib/eh/libunwind.a")
endif()

# rv_elf(<name> <source> [RUNTIME | PICOLIBC | CXX | CXX_EH] [DEPENDS <files>...]
#        [FLAGS <flags>...] [LDFLAGS <flags>...]): compile and link one test ELF; RUNTIME builds it
# against the runtime's headers and links crt0 and the libc, PICOLIBC the same with picolibc, CXX
# a C++ source with picolibc and libc++ as `malm build` does (no exceptions or RTTI), CXX_EH with
# exceptions and RTTI.
function(rv_elf name src)
    cmake_parse_arguments(ARG "RUNTIME;PICOLIBC;CXX;CXX_EH" "" "DEPENDS;FLAGS;LDFLAGS" ${ARGN})
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
    elseif(ARG_CXX OR ARG_CXX_EH)
        set(libs ${cxx_libs})
        set(cxx_flags -fno-exceptions -fno-rtti)
        if(ARG_CXX_EH)
            set(libs ${cxx_eh_libs})
            set(cxx_flags -fexceptions -frtti)
            list(APPEND ld_flags --eh-frame-hdr)
        endif()
        list(PREPEND ARG_FLAGS -O2 -nostdlibinc -isystem "${pl_sysroot}/include/c++/v1"
            -isystem "${pl_sysroot}/include" -ffunction-sections -fdata-sections
            -fbinutils-version=none ${cxx_flags})
        list(APPEND ARG_DEPENDS picolibc_sysroot "${rt_crt0}" ${libs} ${pl_libs})
        set(inputs "${rt_crt0}" "${obj}" ${libs} ${pl_libs})
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

# Rust (issue #34): rustc with the riscv32im target (cmake/RvToolchain.cmake).
set(MALM_RUST_TESTS AUTO CACHE STRING
    "Build and run the Rust test programs: AUTO (if rustc and its riscv32im target are found), ON, OFF")
set_property(CACHE MALM_RUST_TESTS PROPERTY STRINGS AUTO ON OFF)
set(rust_ok FALSE)
if(NOT MALM_RUST_TESTS STREQUAL "OFF")
    if(MALM_RV_RUST_OK)
        set(rust_ok TRUE)
    else()
        set(msg "Rust tests need rustc with the riscv32im-unknown-none-elf target \
(found: MALM_RV_RUSTC=${MALM_RV_RUSTC})")
        if(MALM_RUST_TESTS STREQUAL "ON")
            message(FATAL_ERROR "${msg}")
        endif()
        message(WARNING "${msg}; skipping them (set MALM_RUST_TESTS=OFF to silence this)")
    endif()
endif()
set(rv_rustc "")
if(rust_ok)
    set(rv_rustc "${MALM_RV_RUSTC}")
endif()

# Zig (issue #35): zig 0.17 (cmake/RvToolchain.cmake).
set(MALM_ZIG_TESTS AUTO CACHE STRING
    "Build and run the Zig test programs: AUTO (if zig 0.17 is found), ON, OFF")
set_property(CACHE MALM_ZIG_TESTS PROPERTY STRINGS AUTO ON OFF)
set(zig_ok FALSE)
if(NOT MALM_ZIG_TESTS STREQUAL "OFF")
    if(MALM_RV_ZIG_OK)
        set(zig_ok TRUE)
    else()
        set(msg "Zig tests need zig 0.17 (found: MALM_RV_ZIG=${MALM_RV_ZIG})")
        if(MALM_ZIG_TESTS STREQUAL "ON")
            message(FATAL_ERROR "${msg}")
        endif()
        message(WARNING "${msg}; skipping them (set MALM_ZIG_TESTS=OFF to silence this)")
    endif()
endif()
set(rv_zig "")
if(zig_ok)
    set(rv_zig "${MALM_RV_ZIG}")
endif()

# malm build's C toolchain (issue #28): the runtime built into malm, compiled with this clang; and
# its Rust and Zig toolchains (issues #34, #35) with this rustc and zig, if any.
malm_add_test(NAME rv_toolchain_test LABEL fast SOURCES toolchain_test.cpp
    LIBS malm_rv malm_driver TIMEOUT 300)
target_compile_definitions(rv_toolchain_test PRIVATE MALM_RV_CC="${MALM_RV_CC}"
    MALM_RV_RUSTC="${rv_rustc}"
    MALM_RV_ZIG="${rv_zig}"
    MALM_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/c"
    MALM_CXX_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/cpp"
    MALM_RUST_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/rust"
    MALM_ZIG_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/zig"
    MALM_RV_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs")

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

    # C++ (issue #33): the examples and libc++'s self-check, also with exceptions and RTTI.
    foreach(prog hello sort)
        rv_elf(cxx_example_${prog} "${PROJECT_SOURCE_DIR}/examples/cpp/${prog}.cpp" CXX
            FLAGS -Wall -Wextra -Werror)
        rv_elf(dump_cxx_example_${prog} "${PROJECT_SOURCE_DIR}/examples/cpp/${prog}.cpp" CXX
            FLAGS -Wall -Wextra -Werror
            LDFLAGS --defsym=__malm_dump_regs=1 --defsym=__stack_size=16384)
    endforeach()
    rv_elf(cxx_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/cxx_test.cpp" CXX
        FLAGS -std=c++20 -Wall -Wextra -Werror)
    rv_elf(dump_cxx_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/cxx_test.cpp" CXX
        FLAGS -std=c++20 -Wall -Wextra -Werror LDFLAGS --defsym=__malm_dump_regs=1)
    rv_elf(cxx_eh_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/cxx_test.cpp" CXX_EH
        FLAGS -std=c++20 -Wall -Wextra -Werror)
    rv_elf(dump_cxx_eh_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/cxx_test.cpp" CXX_EH
        FLAGS -std=c++20 -Wall -Wextra -Werror LDFLAGS --defsym=__malm_dump_regs=1)
    set(rv_cxx_cases example_hello sort self_check abort)
    set(rv_cxx_eh_cases self_check abort)
endif()

# Rust (issue #34): the runtime crate, then the examples and the runtime's self-check against it, as
# malm build compiles them (rustc links), with -D warnings.
set(rv_rust_cases "")
if(rust_ok)
    set(rust_flags --target riscv32im-unknown-none-elf --edition 2024 -C opt-level=2 -D warnings)
    set(rust_rlib "${rv_elf_dir}/rust/libmalm.rlib")
    add_custom_command(OUTPUT "${rust_rlib}"
        COMMAND "${MALM_RV_RUSTC}" ${rust_flags} --crate-type rlib --crate-name malm
            "${rt_dir}/rust/malm.rs" -o "${rust_rlib}"
        DEPENDS "${rt_dir}/rust/malm.rs"
        COMMENT "Building the Rust runtime crate"
        VERBATIM)

    # rust_elf(<name> <source> [LDFLAGS <flags>...]): compile and link one Rust test ELF.
    function(rust_elf name src)
        cmake_parse_arguments(ARG "" "" "LDFLAGS" ${ARGN})
        set(elf "${rv_elf_dir}/${name}.elf")
        set(link_args "")
        foreach(flag IN LISTS ARG_LDFLAGS)
            list(APPEND link_args -C "link-arg=${flag}")
        endforeach()
        add_custom_command(OUTPUT "${elf}"
            COMMAND "${MALM_RV_RUSTC}" ${rust_flags} --crate-type bin --extern "malm=${rust_rlib}"
                -C "link-arg=-T${rv_ld_script}" ${link_args} "${src}" -o "${elf}"
            DEPENDS "${src}" "${rust_rlib}" "${rv_ld_script}"
            COMMENT "Building RISC-V test ${name}"
            VERBATIM)
        set(rv_elfs ${rv_elfs} "${elf}" PARENT_SCOPE)
    endfunction()

    foreach(prog hello cat sort)
        rust_elf(rust_example_${prog} "${PROJECT_SOURCE_DIR}/examples/rust/${prog}.rs")
        rust_elf(dump_rust_example_${prog} "${PROJECT_SOURCE_DIR}/examples/rust/${prog}.rs"
            LDFLAGS --defsym=__malm_dump_regs=1 --defsym=__stack_size=16384)
    endforeach()
    rust_elf(rust_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/rust_test.rs")
    rust_elf(dump_rust_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/rust_test.rs"
        LDFLAGS --defsym=__malm_dump_regs=1)
    set(rv_rust_cases example_hello example_cat sort checks panic error exit)
endif()

# Zig (issue #35): the examples and the runtime's self-check, as malm build compiles them (start.zig
# the root module, the program and malm.zig the others). zig has no --defsym: a linker script that
# includes the runtime's defines the symbols (an 8K heap reserve: sort's code is 36 KB, more would
# leave it no heap in 64K).
set(rv_zig_cases "")
if(zig_ok)
    set(zig_rt "${rt_dir}/zig")
    set(zig_dump_ld "${rv_elf_dir}/zig/dump.ld")
    file(WRITE "${zig_dump_ld}" "INCLUDE \"${rv_ld_script}\"\n__malm_dump_regs = 1;\n")
    file(WRITE "${rv_elf_dir}/zig/dump_heap.ld"
        "INCLUDE \"${rv_ld_script}\"\n__malm_dump_regs = 1;\n__stack_size = 8192;\n")

    # zig_elf(<name> <source> [SCRIPT <linker script>]): compile and link one Zig test ELF.
    function(zig_elf name src)
        cmake_parse_arguments(ARG "" "SCRIPT" "" ${ARGN})
        if(NOT ARG_SCRIPT)
            set(ARG_SCRIPT "${rv_ld_script}")
        endif()
        set(elf "${rv_elf_dir}/${name}.elf")
        add_custom_command(OUTPUT "${elf}"
            COMMAND "${MALM_RV_ZIG}" build-exe -target riscv32-freestanding-none
                -mcpu=generic_rv32+m -O ReleaseSafe --cache-dir "${rv_elf_dir}/zig/cache"
                --name ${name} -T "${ARG_SCRIPT}" "-femit-bin=${elf}"
                --dep malm --dep program "-Mroot=${zig_rt}/start.zig"
                --dep malm "-Mprogram=${src}" "-Mmalm=${zig_rt}/malm.zig"
            DEPENDS "${src}" "${zig_rt}/start.zig" "${zig_rt}/malm.zig" "${rv_ld_script}"
                "${ARG_SCRIPT}"
            COMMENT "Building RISC-V test ${name}"
            VERBATIM)
        set(rv_elfs ${rv_elfs} "${elf}" PARENT_SCOPE)
    endfunction()

    foreach(prog hello cat sort)
        zig_elf(zig_example_${prog} "${PROJECT_SOURCE_DIR}/examples/zig/${prog}.zig")
        zig_elf(dump_zig_example_${prog} "${PROJECT_SOURCE_DIR}/examples/zig/${prog}.zig"
            SCRIPT "${rv_elf_dir}/zig/dump_heap.ld")
    endforeach()
    zig_elf(zig_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/zig_test.zig")
    zig_elf(dump_zig_test "${CMAKE_CURRENT_SOURCE_DIR}/programs/zig_test.zig"
        SCRIPT "${zig_dump_ld}")
    set(rv_zig_cases example_hello example_cat sort checks panic error exit)
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
foreach(libc picolibc cxx cxx_eh rust zig)
    foreach(case IN LISTS rv_${libc}_cases)
        add_test(NAME rvref_${libc}_${case}
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/rvref_pipe_test.sh" $<TARGET_FILE:malm>
                "${rv_elf_dir}" "${PROJECT_SOURCE_DIR}/tests/programs" ${case} ${libc})
        set_tests_properties(rvref_${libc}_${case} PROPERTIES LABELS fast TIMEOUT 60)
    endforeach()
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
# C++ (issue #33): the examples and libc++'s self-check (its abort too); with exceptions in emu
# mode only. Rust and Zig (issues #34, #35): the examples and the runtime's self-check, also its
# panic, error and exit. The hellos are fast, the rest slow.
set(rv_diff_specs "")
if(TARGET picolibc_sysroot)
    list(APPEND rv_diff_specs
        "cxx_hello|dump_cxx_example_hello|64K|/dev/null"
        "cxx_sort|dump_cxx_example_sort|64K|${rv_inputs}/sort.in"
        "cxx_self_check|dump_cxx_test|256K|/dev/null|${rv_inputs}/cxx_abort.in"
        "cxx_eh_self_check|dump_cxx_eh_test|256K|--emu-only|/dev/null|${rv_inputs}/cxx_abort.in")
endif()
if(rv_rust_cases)
    list(APPEND rv_diff_specs
        "rust_hello|dump_rust_example_hello|64K|/dev/null"
        "rust_cat|dump_rust_example_cat|64K|${rv_inputs}/hi.in|${PROJECT_SOURCE_DIR}/tests/programs/cat5_input.bin|/dev/null"
        "rust_sort|dump_rust_example_sort|64K|${rv_inputs}/sort.in"
        "rust_self_check|dump_rust_test|256K|/dev/null|${rv_inputs}/panic.in|${rv_inputs}/error.in|${rv_inputs}/exit.in")
endif()
if(rv_zig_cases)
    list(APPEND rv_diff_specs
        "zig_hello|dump_zig_example_hello|64K|/dev/null"
        "zig_cat|dump_zig_example_cat|64K|${rv_inputs}/hi.in|${PROJECT_SOURCE_DIR}/tests/programs/cat5_input.bin|/dev/null"
        "zig_sort|dump_zig_example_sort|64K|${rv_inputs}/sort.in"
        "zig_self_check|dump_zig_test|256K|/dev/null|${rv_inputs}/panic.in|${rv_inputs}/error.in|${rv_inputs}/exit.in")
endif()
foreach(spec IN LISTS rv_diff_specs)
    string(REPLACE "|" ";" fields "${spec}")
    list(POP_FRONT fields case elf ram)
    set(label slow)
    if(case MATCHES "_hello$")
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
    # C++ (issue #33), the M5 criterion for C++, in emu and aot mode; sort in emu mode is fast.
    foreach(case hello sort)
        foreach(mode emu aot)
            set(label slow)
            if(case STREQUAL "sort" AND mode STREQUAL "emu")
                set(label fast)
            endif()
            set(name build_cpp_${case})
            if(mode STREQUAL "aot")
                set(name build_cpp_aot_${case})
            endif()
            add_test(NAME ${name}
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
                    "${MALM_RV_CC}" "${PROJECT_SOURCE_DIR}/examples/cpp"
                    "${PROJECT_SOURCE_DIR}/tests/programs" ${case} ${mode} picolibc)
            set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 1800)
        endforeach()
    endforeach()
endif()
# Rust (issue #34), the M5 criterion for Rust: hello, cat and sort in emu and aot mode; cat in emu
# mode is fast.
if(rv_rust_cases)
    foreach(case hello cat sort)
        foreach(mode emu aot)
            set(label slow)
            if(case STREQUAL "cat" AND mode STREQUAL "emu")
                set(label fast)
            endif()
            set(name build_rust_${case})
            if(mode STREQUAL "aot")
                set(name build_rust_aot_${case})
            endif()
            add_test(NAME ${name}
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
                    "${MALM_RV_RUSTC}" "${PROJECT_SOURCE_DIR}/examples/rust"
                    "${PROJECT_SOURCE_DIR}/tests/programs" ${case} ${mode} rust)
            set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 1800)
        endforeach()
    endforeach()
endif()
# Zig (issue #35), the M5 criterion for Zig: hello and cat, and sort, in emu and aot mode; cat in
# emu mode is fast.
if(rv_zig_cases)
    foreach(case hello cat sort)
        foreach(mode emu aot)
            set(label slow)
            if(case STREQUAL "cat" AND mode STREQUAL "emu")
                set(label fast)
            endif()
            set(name build_zig_${case})
            if(mode STREQUAL "aot")
                set(name build_zig_aot_${case})
            endif()
            add_test(NAME ${name}
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_pipe_test.sh" $<TARGET_FILE:malm>
                    "${MALM_RV_ZIG}" "${PROJECT_SOURCE_DIR}/examples/zig"
                    "${PROJECT_SOURCE_DIR}/tests/programs" ${case} ${mode} zig)
            set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 1800)
        endforeach()
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
