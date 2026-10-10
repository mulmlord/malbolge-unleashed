// riscv-tests environment for user-mode emulators (replaces riscv-test-env's p/riscv_test.h): no
// CSRs, no trap handler, no tohost. The test runs from _start and reports through the exit ecall:
// status 0 on pass, the failing test number (TESTNUM = gp) mod 256 on fail, never 0.
#ifndef MALM_RISCV_TEST_H
#define MALM_RISCV_TEST_H

#define RVTEST_RV32U .macro init; .endm
#define RVTEST_RV64U RVTEST_RV32U
#define RVTEST_RV32M RVTEST_RV32U
#define RVTEST_RV64M RVTEST_RV32U

#define TESTNUM gp

#define RVTEST_CODE_BEGIN                                                                          \
    .section .text.init;                                                                           \
    .align 6;                                                                                      \
    .globl _start;                                                                                 \
    _start:                                                                                        \
    init;

#define RVTEST_CODE_END unimp

#define RVTEST_PASS                                                                                \
    li a0, 0;                                                                                      \
    li a7, 93;                                                                                     \
    ecall

#define RVTEST_FAIL                                                                                \
    andi a0, TESTNUM, 255;                                                                         \
    seqz t0, a0;                                                                                   \
    or a0, a0, t0;                                                                                 \
    li a7, 93;                                                                                     \
    ecall

#define EXTRA_DATA

#define RVTEST_DATA_BEGIN                                                                          \
    EXTRA_DATA                                                                                     \
    .align 4;                                                                                      \
    .global begin_signature;                                                                       \
    begin_signature:

#define RVTEST_DATA_END                                                                            \
    .align 4;                                                                                      \
    .global end_signature;                                                                         \
    end_signature:

#endif
