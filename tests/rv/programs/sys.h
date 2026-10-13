// Freestanding ecall wrappers for the test programs; they run without the RISC-V runtime (sp is set
// by the emulator, there is no crt0 or libc).
#pragma once

static inline long sys3(long n, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline long sys_read(int fd, void* buf, unsigned long n) { return sys3(63, fd, (long)buf, (long)n); }
static inline long sys_write(int fd, const void* buf, unsigned long n) { return sys3(64, fd, (long)buf, (long)n); }
static inline void* sys_brk(void* addr) { return (void*)sys3(214, (long)addr, 0, 0); }
// Built with -DMALM_DUMP_REGS, sys_exit dumps the registers first (ecall 255, docs/rv.md 4).
static inline __attribute__((noreturn)) void sys_exit(int code) {
#ifdef MALM_DUMP_REGS
    sys3(255, 0, 0, 0);
#endif
    sys3(93, code, 0, 0);
    __builtin_unreachable();
}
