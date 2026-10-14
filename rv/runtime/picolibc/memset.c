// memset for picolibc (docs/rv.md section 6), the minimal runtime's (src/string.c). libmalm.a comes
// before picolibc's libc.a when a program is linked, so this one is taken: picolibc's assembly
// memset jumps into the middle of a table of stores, which aot mode cannot translate (docs/rv.md
// section 9).
#include <stdint.h>
#include <string.h>

typedef uint32_t __attribute__((may_alias)) word;

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = dst;
    for (; n > 0 && ((uintptr_t)d & 3) != 0; --n) {
        *d++ = (unsigned char)c;
    }
    uint32_t w = (unsigned char)c * 0x01010101u;
    for (; n >= 4; n -= 4, d += 4) {
        *(word*)d = w;
    }
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}
