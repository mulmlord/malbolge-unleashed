// Self-checking test of the RISC-V runtime (crt0, syscalls, libc), run by rvref_pipe_test.sh with
// stdin "line1\nxyz". Prints "ab", "c" (stdout and stderr in order), "runtime_test: N checks
// passed", then "atexit 2", "atexit 1" (no newline) from the atexit handlers, and exits with
// status 42 through exit(); a failed check prints a FAIL line and the status is 1. With stdin
// "assert\n" it fails an assert instead (status 134). The guest-only parts are under __riscv; the
// rest also builds for the host, where libc checks the expected values.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

static void check(int ok, int line, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL line %d: %s\n", line, what);
    }
}

#define CHECK(e) check((e) != 0, __LINE__, #e)

static void check_fmt(int line, const char* want, int n, const char* got) {
    ++checks;
    if (strcmp(got, want) != 0 || n != (int)strlen(want)) {
        ++failures;
        printf("FAIL line %d: got \"%s\" (%d), want \"%s\"\n", line, got, n, want);
    }
}

#define CHECK_FMT(want, ...)                                                                       \
    do {                                                                                           \
        char buf_[64];                                                                             \
        int n_ = snprintf(buf_, sizeof buf_, __VA_ARGS__);                                         \
        check_fmt(__LINE__, want, n_, buf_);                                                       \
    } while (0)

static int zeroed[16];
static int initialized = 42;
static int constructed;

__attribute__((constructor)) static void construct(void) { constructed = initialized + 1; }

static void test_startup(int argc, char** argv) {
    int all_zero = 1;
    for (size_t i = 0; i < sizeof zeroed / sizeof zeroed[0]; ++i) {
        all_zero &= zeroed[i] == 0;
    }
    CHECK(all_zero);
    CHECK(initialized == 42);
    CHECK(constructed == 43);
    CHECK(argc == 0 || argv[argc] == NULL);
    CHECK(((uintptr_t)__builtin_frame_address(0) & 15) == 0);
}

static void test_string(void) {
    char buf[32];
    CHECK(strlen("") == 0 && strlen("abc") == 3);
    CHECK(strnlen("abcdef", 4) == 4 && strnlen("ab", 4) == 2);
    CHECK(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("b", "abc") > 0);
    CHECK(strcmp("\xff", "a") > 0); // unsigned char comparison
    CHECK(strncmp("abcx", "abcy", 3) == 0 && strncmp("abcx", "abcy", 4) < 0);
    CHECK(strcmp(strcpy(buf, "hello"), "hello") == 0);
    CHECK(strcmp(strcat(buf, ", world"), "hello, world") == 0);
    CHECK(strcmp(strncat(buf, "!!!", 1), "hello, world!") == 0);
    CHECK(strchr(buf, 'o') == buf + 4 && strrchr(buf, 'o') == buf + 8);
    CHECK(strchr(buf, '\0') == buf + 13 && strchr(buf, 'z') == NULL);
    CHECK(strstr(buf, "world") == buf + 7 && strstr(buf, "") == buf && !strstr(buf, "worlds"));
    memset(buf, 'x', sizeof buf);
    strncpy(buf, "ab", 4);
    CHECK(memcmp(buf, "ab\0\0x", 5) == 0);
    CHECK(memcmp("abc", "abd", 3) < 0 && memcmp("abc", "abd", 2) == 0);
    CHECK(memchr("abc", 'c', 3) != NULL && memchr("abc", 'c', 2) == NULL);

    // Word and byte paths of memcpy, memset and memmove, at every alignment.
    unsigned char src[40];
    unsigned char dst[40];
    for (int i = 0; i < 40; ++i) {
        src[i] = (unsigned char)(i * 7 + 1);
    }
    int ok = 1;
    for (int off = 0; off < 4; ++off) {
        for (int len = 0; len < 20; ++len) {
            memset(dst, 0xee, sizeof dst);
            memcpy(dst + off, src + 4, (size_t)len);
            ok &= memcmp(dst + off, src + 4, (size_t)len) == 0 && dst[off + len] == 0xee;
            memset(dst + off, 0x5a, (size_t)len);
            for (int i = 0; i < len; ++i) {
                ok &= dst[off + i] == 0x5a;
            }
            ok &= dst[off + len] == 0xee && (off == 0 || dst[off - 1] == 0xee);
        }
    }
    CHECK(ok);
    memcpy(dst, src, sizeof dst);
    memmove(dst + 3, dst, 20);
    CHECK(memcmp(dst + 3, src, 20) == 0);
    memcpy(dst, src, sizeof dst);
    memmove(dst, dst + 5, 20);
    CHECK(memcmp(dst, src + 5, 20) == 0);

    char* copy = strdup("dup");
    CHECK(copy && strcmp(copy, "dup") == 0);
    free(copy);
}

static void test_ctype(void) {
    CHECK(isdigit('7') && !isdigit('a') && isxdigit('F') && !isxdigit('g'));
    CHECK(isalpha('q') && isalnum('Q') && isalnum('5') && !isalpha('_'));
    CHECK(isspace(' ') && isspace('\n') && isspace('\v') && !isspace('x') && isblank('\t'));
    CHECK(ispunct('!') && !ispunct('a') && !ispunct(' ') && isprint(' ') && !isgraph(' '));
    CHECK(iscntrl('\0') && iscntrl(0x7f) && !iscntrl('a') && !isalpha(EOF) && !isprint(0xe9));
    CHECK(toupper('a') == 'A' && toupper('1') == '1' && tolower('Z') == 'z' && tolower(EOF) == EOF);
}

static void test_strtol(void) {
    char* end;
    CHECK(strtol("  -123xyz", &end, 10) == -123 && *end == 'x');
    CHECK(strtol("0x1F", &end, 0) == 31 && *end == '\0');
    CHECK(strtol("0x1F", &end, 16) == 31 && *end == '\0');
    CHECK(strtol("017", &end, 0) == 15 && strtol("z", &end, 36) == 35);
    CHECK(strtol("0xg", &end, 16) == 0 && *end == 'x');
    CHECK(strtol("+", &end, 10) == 0 && *end == '+');
    CHECK(strtol("   ", &end, 10) == 0 && *end == ' ');
#if LONG_MAX == 0x7fffffff
    errno = 0;
    CHECK(strtol("2147483647", NULL, 10) == LONG_MAX && errno == 0);
    CHECK(strtol("-2147483648", NULL, 10) == LONG_MIN && errno == 0);
    CHECK(strtol("2147483648", NULL, 10) == LONG_MAX && errno == ERANGE);
    errno = 0;
    CHECK(strtol("-2147483649", NULL, 10) == LONG_MIN && errno == ERANGE);
    errno = 0;
    CHECK(strtoul("4294967295", NULL, 10) == ULONG_MAX && errno == 0);
    CHECK(strtoul("-1", NULL, 10) == ULONG_MAX && errno == 0);
    CHECK(strtoul("99999999999", &end, 10) == ULONG_MAX && errno == ERANGE && *end == '\0');
#endif
    CHECK(atoi("42") == 42 && atoi(" -7") == -7 && atol("100") == 100);
    CHECK(abs(-5) == 5 && labs(-6) == 6);
}

static void test_printf(void) {
    CHECK_FMT("42", "%d", 42);
    CHECK_FMT("-42", "%i", -42);
    CHECK_FMT("+42  42", "%+d % d", 42, 42);
    CHECK_FMT("   42|42   |00042|-0042", "%5d|%-5d|%05d|%05d", 42, 42, 42, -42);
    CHECK_FMT("  042|", "%5.3d|", 42);
    CHECK_FMT("|0", "%.0d|%d", 0, 0);
    CHECK_FMT("-2147483648 4294967295", "%d %u", INT_MIN, UINT_MAX);
    CHECK_FMT("ff FF 0xff 0XFF 0", "%x %X %#x %#X %#x", 255, 255, 255, 255, 0);
    CHECK_FMT("17 017 0 0", "%o %#o %#o %#.0o", 15, 15, 0, 0);
    CHECK_FMT("-1 255 -1 65535", "%hhd %hhu %hd %hu", 255, 255, 65535, 65535);
    CHECK_FMT("-9223372036854775808", "%lld", LLONG_MIN);
    CHECK_FMT("18446744073709551615", "%llu", ULLONG_MAX);
    CHECK_FMT("123456789abcdef0", "%llx", 0x123456789abcdef0ULL);
    CHECK_FMT("1777777777777777777777", "%llo", ULLONG_MAX);
    CHECK_FMT("-1 12345 -5 2147483647", "%jd %zu %td %ld", (intmax_t)-1, (size_t)12345,
              (ptrdiff_t)-5, (long)INT_MAX);
    CHECK_FMT("    7|7    |007|7", "%*d|%*d|%.*d|%.*d", 5, 7, -5, 7, 3, 7, -1, 7);
    CHECK_FMT("x  x|x  |", "%c%3c|%-3c|", 'x', 'x', 'x');
    const char* volatile null = NULL;
    CHECK_FMT("hello hel    hi|hi   |(null)", "%s %.3s %5s|%-5s|%s", "hello", "hello", "hi", "hi",
              null);
    CHECK_FMT("100% a-1b", "%d%% a%db", 100, -1);
    CHECK_FMT("0x1000", "%p", (void*)0x1000);
#ifdef __riscv
    CHECK_FMT("%f 1", "%f %d", 1.5, 1); // unsupported, the argument is skipped
#endif

    char buf[8];
    CHECK(snprintf(buf, 4, "%d", 123456) == 6 && strcmp(buf, "123") == 0);
    CHECK(snprintf(NULL, 0, "%s", "abc") == 3);
    CHECK(sprintf(buf, "%s-%d", "ab", 1) == 4 && strcmp(buf, "ab-1") == 0);
}

static void test_divdi3(void) {
    volatile long long a = -1234567890123LL;
    volatile long long b = 1000;
    volatile unsigned long long u = ULLONG_MAX;
    volatile unsigned long long v = 0x100000001ULL;
    CHECK(a / b == -1234567890LL && a % b == -123);
    CHECK(-a / -b == -1234567890LL && a / -b == 1234567890LL && -a % -b == 123);
    CHECK(u / 3 == 0x5555555555555555ULL && u % 7 == 1);
    CHECK(u / v == 0xffffffffULL && u % v == 0);
    CHECK(0x123456789abcdef0ULL / v == 0x12345678ULL && 0x123456789abcdef0ULL % v == 0x88888878ULL);
    CHECK(u / u == 1 && (u - 1) / u == 0 && 5ULL / v == 0);
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a;
    int y = *(const int*)b;
    return (x > y) - (x < y);
}

static uint32_t rng = 12345;

static uint32_t next_random(void) {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void test_qsort(void) {
    int values[100];
    for (int i = 0; i < 100; ++i) {
        values[i] = (int)(next_random() % 50) - 25;
    }
    qsort(values, 100, sizeof values[0], cmp_int);
    int sorted = 1;
    for (int i = 1; i < 100; ++i) {
        sorted &= values[i - 1] <= values[i];
    }
    CHECK(sorted);
    int one = 7;
    qsort(&one, 1, sizeof one, cmp_int);
    qsort(NULL, 0, sizeof one, cmp_int);
    CHECK(one == 7);
}

// The allocator is called through volatile pointers: clang knows malloc and friends, and may drop
// an allocation that is only compared or read back, or assume that it leaves errno alone.
static void* (*volatile malloc_)(size_t) = malloc;
static void* (*volatile calloc_)(size_t, size_t) = calloc;
static void* (*volatile realloc_)(void*, size_t) = realloc;
static void (*volatile free_)(void*) = free;

static void test_malloc(void) {
    enum { kBlocks = 48 };
    unsigned char* blocks[kBlocks] = {0};
    size_t sizes[kBlocks];
    int ok = 1;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < kBlocks; ++i) {
            if (blocks[i] && next_random() % 2) {
                for (size_t j = 0; j < sizes[i]; ++j) {
                    ok &= blocks[i][j] == (unsigned char)(i * 31 + (int)j);
                }
                free_(blocks[i]);
                blocks[i] = NULL;
            }
            if (!blocks[i]) {
                sizes[i] =
                    next_random() % 8 == 0 ? 5000 + next_random() % 3000 : next_random() % 300;
                blocks[i] = malloc_(sizes[i]);
                ok &= blocks[i] != NULL && ((uintptr_t)blocks[i] & 15) == 0;
                for (size_t j = 0; blocks[i] && j < sizes[i]; ++j) {
                    blocks[i][j] = (unsigned char)(i * 31 + (int)j);
                }
            }
        }
    }
    for (int i = 0; i < kBlocks; ++i) {
        for (size_t j = 0; blocks[i] && j < sizes[i]; ++j) {
            ok &= blocks[i][j] == (unsigned char)(i * 31 + (int)j);
        }
        free_(blocks[i]);
    }
    CHECK(ok);

    char* p = malloc_(10);
    CHECK(p != NULL);
    strcpy(p, "realloc");
    CHECK(realloc_(p, 12) == p); // fits in its 16-byte unit
    p = realloc_(p, 1000);
    CHECK(p && strcmp(p, "realloc") == 0);
    free_(p);
    // calloc zeroes reused memory.
    p = malloc_(400);
    CHECK(p != NULL);
    memset(p, 0xff, 400);
    free_(p);
    int* z = calloc_(100, sizeof(int));
    int all_zero = z != NULL;
    for (int i = 0; z && i < 100; ++i) {
        all_zero &= z[i] == 0;
    }
    CHECK(all_zero);
    free_(z);
    free_(NULL);
    errno = 0;
    CHECK(malloc_(SIZE_MAX) == NULL && errno == ENOMEM);
    errno = 0;
    CHECK(calloc_(SIZE_MAX / 2, 4) == NULL && errno == ENOMEM);
#ifdef __riscv
    errno = 0;
    CHECK(malloc_(2u << 20) == NULL && errno == ENOMEM); // more than the 1 MiB of guest RAM

    // Everything is free again and merged: the whole heap is one block, so allocating all of it
    // does not move the break.
    extern char _end[];
    char* top = sbrk(0);
    size_t heap = (size_t)(top - (char*)(((uintptr_t)_end + 15) & ~(uintptr_t)15));
    p = malloc_(heap - 16);
    CHECK(p != NULL && sbrk(0) == top);
    free_(p);
#endif
}

#ifdef __riscv
extern char* __malm_stack_top;
extern char __stack_size[];

static void test_syscalls(void) {
    char c = 'x';
    errno = 0;
    CHECK(write(5, &c, 1) == -1 && errno == EBADF);
    CHECK(read(1, &c, 1) == -1 && errno == EBADF);
    CHECK(write(1, (void*)0xfffffff0, 1) == -1 && errno == EFAULT);

    // The heap stops __stack_size (64 KiB by default) below the initial stack pointer.
    char* limit = __malm_stack_top - (uintptr_t)__stack_size;
    char* old = sbrk(0);
    CHECK((uintptr_t)__stack_size == 0x10000);
    errno = 0;
    CHECK(brk(limit + 16) == -1 && errno == ENOMEM && sbrk(0) == old);
    CHECK(brk(limit) == 0 && sbrk(0) == limit);
    CHECK(sbrk(1) == (void*)-1 && sbrk(-16) == limit && sbrk(0) == limit - 16);
    CHECK(brk(old) == 0 && sbrk(0) == old);
    extern char _end[];
    CHECK(brk(_end - 16) == -1); // below the initial break
}
#endif

static void test_stdin(void) {
    char line[16];
    CHECK(fgets(line, sizeof line, stdin) && strcmp(line, "line1\n") == 0);
    CHECK(getchar() == 'x');
    CHECK(ungetc('x', stdin) == 'x');
#ifdef __riscv
    CHECK(ungetc('y', stdin) == EOF); // one byte of pushback (glibc has more)
#endif
    char buf[10];
    CHECK(fread(buf, 1, sizeof buf, stdin) == 3 && memcmp(buf, "xyz", 3) == 0);
    CHECK(feof(stdin) && !ferror(stdin) && getchar() == EOF && !fgets(line, sizeof line, stdin));
    clearerr(stdin);
    CHECK(!feof(stdin));
}

static void handler1(void) { fputs("atexit 1", stdout); } // no newline: exit flushes it
static void handler2(void) { puts("atexit 2"); }

__attribute__((noreturn)) static void finish(void) {
    printf("runtime_test: %d checks passed\n", checks - failures);
    exit(failures == 0 ? 42 : 1);
}

int main(int argc, char** argv) {
    // stdout is buffered and stderr is not; both reach the same stream in order.
    printf("a");
    fputs("b\n", stderr);
    printf("c\n");

    int c = getchar();
    if (c == 'a') {
        assert(c == 'b');
    }
    ungetc(c, stdin);

    CHECK(atexit(handler1) == 0 && atexit(handler2) == 0);
    test_startup(argc, argv);
    test_string();
    test_ctype();
    test_strtol();
    test_printf();
    test_divdi3();
    test_qsort();
    test_malloc();
#ifdef __riscv
    test_syscalls();
#endif
    test_stdin();
    finish();
}
