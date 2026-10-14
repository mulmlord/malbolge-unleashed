# The RISC-V toolchain on the build machine (docs/rv.md 6, 7): clang with the riscv32 target
# compiles, ld.lld links (called directly, so no driver linker search is involved), an archiver
# builds the picolibc runtime (rv/runtime/CMakeLists.txt). Versioned names cover Ubuntu's clang-18
# / lld-18.
# Sets MALM_RV_TOOLCHAIN_OK if clang and ld.lld build a riscv32 program.
find_program(MALM_RV_CC NAMES clang clang-22 clang-21 clang-20 clang-19 clang-18)
if(MALM_RV_CC)
    file(REAL_PATH "${MALM_RV_CC}" rv_cc_real)
    cmake_path(GET rv_cc_real PARENT_PATH rv_cc_dir)
endif()
find_program(MALM_RV_LD NAMES ld.lld ld.lld-22 ld.lld-21 ld.lld-20 ld.lld-19 ld.lld-18
    HINTS "${rv_cc_dir}")
# ld.lld needs no archive index, so any ar will do.
find_program(MALM_RV_AR NAMES llvm-ar llvm-ar-22 llvm-ar-21 llvm-ar-20 llvm-ar-19 llvm-ar-18 ar
    HINTS "${rv_cc_dir}")
set(MALM_RV_FLAGS --target=riscv32-unknown-elf -march=rv32im_zifencei -mabi=ilp32)
set(MALM_RV_TOOLCHAIN_OK FALSE)
if(MALM_RV_CC AND MALM_RV_LD)
    set(probe "${CMAKE_CURRENT_BINARY_DIR}/rv_probe")
    file(WRITE "${probe}.S" "_start: ecall\n")
    execute_process(COMMAND "${MALM_RV_CC}" ${MALM_RV_FLAGS} -c "${probe}.S" -o "${probe}.o"
        RESULT_VARIABLE cc_rc OUTPUT_QUIET ERROR_QUIET)
    if(cc_rc EQUAL 0)
        execute_process(COMMAND "${MALM_RV_LD}" "${probe}.o" -o "${probe}.elf"
            RESULT_VARIABLE ld_rc OUTPUT_QUIET ERROR_QUIET)
        if(ld_rc EQUAL 0)
            set(MALM_RV_TOOLCHAIN_OK TRUE)
        endif()
    endif()
endif()
