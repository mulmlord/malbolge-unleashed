// Built with -ffreestanding, so the compiler does not turn these loops back into calls to
// themselves. memcpy and memset move words when they can: every guest instruction is expensive.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t __attribute__((may_alias)) word;

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
        for (; n >= 4; n -= 4, d += 4, s += 4) {
            *(word*)d = *(const word*)s;
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    if ((uintptr_t)d - (uintptr_t)s >= n) {
        return memcpy(dst, src, n); // no overlap, or d before s
    }
    while (n--) {
        d[n] = s[n];
    }
    return dst;
}

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

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            return x[i] - y[i];
        }
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == (unsigned char)c) {
            return (void*)(p + i);
        }
    }
    return NULL;
}

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) {
        ++p;
    }
    return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) {
        ++n;
    }
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (; n > 0; --n, ++a, ++b) {
        if (*a != *b || !*a) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) {
    }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; ++i) {
        dst[i] = src[i];
    }
    for (; i < n; ++i) {
        dst[i] = '\0';
    }
    return dst;
}

char* strcat(char* dst, const char* src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* d = dst + strlen(dst);
    for (; n > 0 && *src; --n) {
        *d++ = *src++;
    }
    *d = '\0';
    return dst;
}

char* strchr(const char* s, int c) {
    for (;; ++s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; ++s) {
        if (*s == (char)c) {
            last = s;
        }
        if (!*s) {
            return (char*)last;
        }
    }
}

char* strstr(const char* haystack, const char* needle) {
    size_t n = strlen(needle);
    for (; *haystack; ++haystack) {
        if (strncmp(haystack, needle, n) == 0) {
            return (char*)haystack;
        }
    }
    return n == 0 ? (char*)haystack : NULL;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}
