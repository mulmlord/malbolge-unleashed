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

# Rust (docs/rv.md 6): rustc with the riscv32im-unknown-none-elf target installed (rustup target add
# riscv32im-unknown-none-elf), for the Rust tests. rustup puts rustc in ~/.cargo/bin, which need not
# be in PATH. Sets MALM_RV_RUST_OK if the target's core library is there.
find_program(MALM_RV_RUSTC NAMES rustc HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
set(MALM_RV_RUST_OK FALSE)
if(MALM_RV_RUSTC)
    execute_process(
        COMMAND "${MALM_RV_RUSTC}" --print target-libdir --target riscv32im-unknown-none-elf
        RESULT_VARIABLE rustc_rc OUTPUT_VARIABLE rust_libdir ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(rustc_rc EQUAL 0)
        file(GLOB rust_core "${rust_libdir}/libcore-*.rlib")
        if(rust_core)
            set(MALM_RV_RUST_OK TRUE)
        endif()
    endif()
endif()

# Zig (docs/rv.md 6): zig 0.17, whose standard library the Zig runtime is written against, for the
# Zig tests. Sets MALM_RV_ZIG_OK if it is that version.
find_program(MALM_RV_ZIG NAMES zig)
set(MALM_RV_ZIG_OK FALSE)
if(MALM_RV_ZIG)
    execute_process(COMMAND "${MALM_RV_ZIG}" version
        RESULT_VARIABLE zig_rc OUTPUT_VARIABLE zig_version ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(zig_rc EQUAL 0 AND zig_version MATCHES "^0\\.17\\.")
        set(MALM_RV_ZIG_OK TRUE)
    endif()
endif()
