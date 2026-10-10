// 64-bit division, which RV32IM lacks: the libgcc / compiler-rt helpers that clang calls for
// (unsigned) long long `/` and `%`. Division by zero (undefined in C) returns without trapping.
#include <stdint.h>

static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t* rem) {
    if (d == 0) {
        *rem = n;
        return ~(uint64_t)0;
    }
    if (d > n) {
        *rem = n;
        return 0;
    }
    if ((n >> 32) == 0) {
        *rem = (uint32_t)n % (uint32_t)d;
        return (uint32_t)n / (uint32_t)d;
    }
    // Shift-subtract, one quotient bit per step from the highest one possible.
    int shift = __builtin_clzll(d) - __builtin_clzll(n);
    d <<= shift;
    uint64_t q = 0;
    for (; shift >= 0; --shift) {
        q <<= 1;
        if (n >= d) {
            n -= d;
            q |= 1;
        }
        d >>= 1;
    }
    *rem = n;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    uint64_t r;
    return udivmod(n, d, &r);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r;
    udivmod(n, d, &r);
    return r;
}

static uint64_t magnitude(int64_t x) { return x < 0 ? 0 - (uint64_t)x : (uint64_t)x; }

int64_t __divdi3(int64_t n, int64_t d) {
    uint64_t r;
    uint64_t q = udivmod(magnitude(n), magnitude(d), &r);
    return (int64_t)((n < 0) != (d < 0) ? 0 - q : q);
}

int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    udivmod(magnitude(n), magnitude(d), &r);
    return (int64_t)(n < 0 ? 0 - r : r);
}
