// The printf family (the subset in stdio.h). A floating-point conversion is printed as written,
// with its argument skipped, so the output shows that it is unsupported.
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Where the output goes: a stream, or a string of size cap (terminated if cap > 0).
struct out {
    FILE* f;
    char* s;
    size_t cap;
    size_t len; // characters produced, including those that did not fit
    bool error;
};

static void emit(struct out* o, const char* p, size_t n) {
    if (o->f) {
        o->error |= n > 0 && fwrite(p, 1, n, o->f) != n;
    } else {
        for (size_t i = 0; i < n && o->len + i + 1 < o->cap; ++i) {
            o->s[o->len + i] = p[i];
        }
    }
    o->len += n;
}

static void pad(struct out* o, char c, int n) {
    for (; n > 0; --n) {
        emit(o, &c, 1);
    }
}

enum length { kHH, kH, kInt, kLL };

struct spec {
    bool left, plus, space, alt, zero;
    int width;
    int prec; // -1 if not given
    enum length length;
};

static void put_field(struct out* o, const struct spec* sp, const char* p, size_t n) {
    int fill = sp->width > (int)n ? sp->width - (int)n : 0;
    if (!sp->left) {
        pad(o, ' ', fill);
    }
    emit(o, p, n);
    if (sp->left) {
        pad(o, ' ', fill);
    }
}

static void put_int(struct out* o, const struct spec* sp, uint64_t v, char sign, unsigned base,
                    bool upper) {
    const char* xdigits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24]; // 22 octal digits for 64 bits
    char* end = buf + sizeof buf;
    char* p = end;
    bool nonzero = v != 0;
    if ((v >> 32) == 0) {
        for (uint32_t w = (uint32_t)v; w != 0; w /= base) { // 32-bit division where possible
            *--p = xdigits[w % base];
        }
    } else {
        for (; v != 0; v /= base) {
            *--p = xdigits[v % base];
        }
    }
    int n = (int)(end - p);
    int prec = sp->prec < 0 ? 1 : sp->prec;
    int zeros = prec > n ? prec - n : 0;
    char prefix[3];
    int plen = 0;
    if (sign) {
        prefix[plen++] = sign;
    }
    if (sp->alt && base == 16 && nonzero) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }
    if (sp->alt && base == 8 && zeros == 0) {
        zeros = 1;
    }
    int fill = sp->width - (plen + zeros + n);
    if (fill > 0 && sp->zero && !sp->left && sp->prec < 0) {
        zeros += fill;
        fill = 0;
    }
    if (!sp->left) {
        pad(o, ' ', fill);
    }
    emit(o, prefix, (size_t)plen);
    pad(o, '0', zeros);
    emit(o, p, (size_t)n);
    if (sp->left) {
        pad(o, ' ', fill);
    }
}

static uint64_t arg_unsigned(va_list* ap, enum length length) {
    switch (length) {
    case kHH:
        return (unsigned char)va_arg(*ap, unsigned);
    case kH:
        return (unsigned short)va_arg(*ap, unsigned);
    case kInt:
        return va_arg(*ap, unsigned);
    case kLL:
        return va_arg(*ap, unsigned long long);
    }
    return 0;
}

static int64_t arg_signed(va_list* ap, enum length length) {
    switch (length) {
    case kHH:
        return (signed char)va_arg(*ap, int);
    case kH:
        return (short)va_arg(*ap, int);
    case kInt:
        return va_arg(*ap, int);
    case kLL:
        return va_arg(*ap, long long);
    }
    return 0;
}

static int width_arg(const char** fmt, va_list* ap) {
    if (**fmt == '*') {
        ++*fmt;
        return va_arg(*ap, int);
    }
    int n = 0;
    for (; **fmt >= '0' && **fmt <= '9'; ++*fmt) {
        n = n * 10 + (**fmt - '0');
    }
    return n;
}

static void format(struct out* o, const char* fmt, va_list ap_in) {
    va_list ap;
    va_copy(ap, ap_in);
    while (*fmt) {
        const char* pct = strchr(fmt, '%');
        if (!pct) {
            emit(o, fmt, strlen(fmt));
            break;
        }
        emit(o, fmt, (size_t)(pct - fmt));
        fmt = pct + 1;

        struct spec sp = {.prec = -1, .length = kInt};
        for (;; ++fmt) {
            if (*fmt == '-') {
                sp.left = true;
            } else if (*fmt == '+') {
                sp.plus = true;
            } else if (*fmt == ' ') {
                sp.space = true;
            } else if (*fmt == '#') {
                sp.alt = true;
            } else if (*fmt == '0') {
                sp.zero = true;
            } else {
                break;
            }
        }
        sp.width = width_arg(&fmt, &ap);
        if (sp.width < 0) {
            sp.left = true;
            sp.width = -sp.width;
        }
        if (*fmt == '.') {
            ++fmt;
            sp.prec = width_arg(&fmt, &ap);
            if (sp.prec < 0) {
                sp.prec = -1;
            }
        }
        bool long_double = false;
        if (fmt[0] == 'h' && fmt[1] == 'h') {
            sp.length = kHH;
            fmt += 2;
        } else if (fmt[0] == 'l' && fmt[1] == 'l') {
            sp.length = kLL;
            fmt += 2;
        } else if (*fmt == 'h') {
            sp.length = kH;
            ++fmt;
        } else if (*fmt == 'j') {
            sp.length = kLL; // intmax_t is 64 bits
            ++fmt;
        } else if (*fmt == 'l' || *fmt == 'z' || *fmt == 't') {
            ++fmt; // long, size_t and ptrdiff_t are 32 bits, like int
        } else if (*fmt == 'L') {
            long_double = true;
            ++fmt;
        }

        char conv = *fmt;
        if (conv == '\0') {
            emit(o, pct, (size_t)(fmt - pct));
            break;
        }
        ++fmt;
        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v = arg_signed(&ap, sp.length);
            char sign = v < 0 ? '-' : sp.plus ? '+' : sp.space ? ' ' : 0;
            put_int(o, &sp, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, sign, 10, false);
            break;
        }
        case 'u':
            put_int(o, &sp, arg_unsigned(&ap, sp.length), 0, 10, false);
            break;
        case 'o':
            put_int(o, &sp, arg_unsigned(&ap, sp.length), 0, 8, false);
            break;
        case 'x':
        case 'X':
            put_int(o, &sp, arg_unsigned(&ap, sp.length), 0, 16, conv == 'X');
            break;
        case 'p':
            sp.alt = true;
            put_int(o, &sp, (uintptr_t)va_arg(ap, void*), 0, 16, false);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_field(o, &sp, &c, 1);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) {
                s = "(null)";
            }
            put_field(o, &sp, s, sp.prec < 0 ? strlen(s) : strnlen(s, (size_t)sp.prec));
            break;
        }
        case '%':
            emit(o, "%", 1);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            if (long_double) {
                (void)va_arg(ap, long double);
            } else {
                (void)va_arg(ap, double);
            }
            emit(o, pct, (size_t)(fmt - pct));
            break;
        default:
            emit(o, pct, (size_t)(fmt - pct));
            break;
        }
    }
    va_end(ap);
    if (o->cap > 0) {
        o->s[o->len < o->cap ? o->len : o->cap - 1] = '\0';
    }
}

static int result(const struct out* o) { return o->error ? -1 : (int)o->len; }

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    struct out o = {.f = f};
    format(&o, fmt, ap);
    return result(&o);
}

int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int vsnprintf(char* s, size_t n, const char* fmt, va_list ap) {
    struct out o = {.s = s, .cap = n};
    format(&o, fmt, ap);
    return result(&o);
}

int vsprintf(char* s, const char* fmt, va_list ap) { return vsnprintf(s, SIZE_MAX, fmt, ap); }

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, SIZE_MAX, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char* s, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}
