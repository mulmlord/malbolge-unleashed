// ecall wrappers (docs/rv.md section 4) and the program break.
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

enum { SYS_read = 63, SYS_write = 64, SYS_exit = 93, SYS_brk = 214, SYS_dump_regs = 255 };

int errno;
char* __malm_stack_top; // the initial sp, set by crt0

extern char __stack_size[]; // link.ld
// Linking with --defsym=__malm_dump_regs=1 makes _exit dump the registers first (docs/rv.md 6).
__attribute__((weak)) extern char __malm_dump_regs[];

static long syscall3(long n, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

// Linux convention: results in [-4095, -1] are negated errno values.
static long check(long r) {
    if ((unsigned long)r > -4096UL) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t read(int fd, void* buf, size_t count) {
    return check(syscall3(SYS_read, fd, (long)buf, (long)count));
}

ssize_t write(int fd, const void* buf, size_t count) {
    return check(syscall3(SYS_write, fd, (long)buf, (long)count));
}

void _exit(int status) {
    if (__malm_dump_regs) {
        syscall3(SYS_dump_regs, 0, 0, 0);
    }
    syscall3(SYS_exit, status, 0, 0);
    __builtin_unreachable();
}

static uintptr_t heap_limit(void) {
    uintptr_t top = (uintptr_t)__malm_stack_top;
    uintptr_t reserve = (uintptr_t)__stack_size;
    return top > reserve ? top - reserve : 0;
}

static char* cur_break;

int brk(void* addr) {
    if ((uintptr_t)addr > heap_limit()) {
        errno = ENOMEM;
        return -1;
    }
    cur_break = (char*)syscall3(SYS_brk, (long)addr, 0, 0);
    if (cur_break != addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void* sbrk(intptr_t increment) {
    if (!cur_break) {
        cur_break = (char*)syscall3(SYS_brk, 0, 0, 0);
    }
    char* old = cur_break;
    uintptr_t want = (uintptr_t)old + (uintptr_t)increment;
    if (increment > 0 ? want < (uintptr_t)old : want > (uintptr_t)old) {
        errno = ENOMEM;
        return (void*)-1;
    }
    if (increment != 0 && brk((void*)want) != 0) {
        return (void*)-1;
    }
    return old;
}
