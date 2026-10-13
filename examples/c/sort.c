// Sorts the lines of stdin bytewise, like `LC_ALL=C sort`: lines may hold any bytes, a last line
// without a newline gets one. Exit status 1 with a message on stderr if memory runs out.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct line {
    const char* text;
    size_t len;
};

static int compare(const void* a, const void* b) {
    const struct line* x = a;
    const struct line* y = b;
    int c = memcmp(x->text, y->text, x->len < y->len ? x->len : y->len);
    if (c != 0) {
        return c;
    }
    return (x->len > y->len) - (x->len < y->len);
}

static void* grow(void* p, size_t* cap, size_t size) {
    *cap = *cap ? *cap * 2 : 64;
    p = realloc(p, *cap * size);
    if (p == NULL) {
        fputs("sort: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

int main(void) {
    // All of stdin, then the lines in it.
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        if (len == cap) {
            buf = grow(buf, &cap, 1);
        }
        size_t n = fread(buf + len, 1, cap - len, stdin);
        if (n == 0) {
            break;
        }
        len += n;
    }
    struct line* lines = NULL;
    size_t count = 0;
    size_t lines_cap = 0;
    for (size_t start = 0; start < len;) {
        const char* nl = memchr(buf + start, '\n', len - start);
        size_t end = nl ? (size_t)(nl - buf) : len;
        if (count == lines_cap) {
            lines = grow(lines, &lines_cap, sizeof *lines);
        }
        lines[count++] = (struct line){buf + start, end - start};
        start = end + 1;
    }
    qsort(lines, count, sizeof *lines, compare);
    for (size_t i = 0; i < count; ++i) {
        fwrite(lines[i].text, 1, lines[i].len, stdout);
        putchar('\n');
    }
    return 0;
}
