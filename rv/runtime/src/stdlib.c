#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static int digit_value(int c) {
    if (isdigit(c)) {
        return c - '0';
    }
    if (islower(c)) {
        return c - 'a' + 10;
    }
    if (isupper(c)) {
        return c - 'A' + 10;
    }
    return 99;
}

// The common part of strtol and strtoul: the magnitude after an optional sign, saturated at
// ULONG_MAX with *overflow set.
static unsigned long scan(const char* s, char** end, int base, bool* neg, bool* overflow) {
    const char* p = s;
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    *neg = false;
    *overflow = false;
    if (*p == '+' || *p == '-') {
        *neg = *p == '-';
        ++p;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit_value((unsigned char)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        errno = EINVAL;
        if (end) {
            *end = (char*)s;
        }
        return 0;
    }
    const char* digits = p;
    unsigned long v = 0;
    for (int d; (d = digit_value((unsigned char)*p)) < base; ++p) {
        if (v > (ULONG_MAX - (unsigned long)d) / (unsigned long)base) {
            *overflow = true;
        } else {
            v = v * (unsigned long)base + (unsigned long)d;
        }
    }
    if (end) {
        *end = (char*)(p == digits ? s : p);
    }
    return *overflow ? ULONG_MAX : v;
}

unsigned long strtoul(const char* s, char** end, int base) {
    bool neg;
    bool overflow;
    unsigned long v = scan(s, end, base, &neg, &overflow);
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? 0 - v : v;
}

long strtol(const char* s, char** end, int base) {
    bool neg;
    bool overflow;
    unsigned long v = scan(s, end, base, &neg, &overflow);
    if (overflow || v > (neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)(0 - v) : (long)v;
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }

// qsort is a heapsort: O(n log n) comparisons, no recursion, no extra memory.
static void swap(char* a, char* b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char* base, size_t root, size_t count, size_t size,
                      int (*cmp)(const void*, const void*)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= count) {
            return;
        }
        if (child + 1 < count && cmp(base + child * size, base + (child + 1) * size) < 0) {
            ++child;
        }
        if (cmp(base + root * size, base + child * size) >= 0) {
            return;
        }
        swap(base + root * size, base + child * size, size);
        root = child;
    }
}

void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*)) {
    char* b = base;
    for (size_t i = count / 2; i-- > 0;) {
        sift_down(b, i, count, size, cmp);
    }
    for (size_t end = count; end-- > 1;) {
        swap(b, b + end * size, size);
        sift_down(b, 0, end, size, cmp);
    }
}
