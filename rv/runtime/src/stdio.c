// Streams over read and write. read blocks until it has all the bytes asked for (docs/rv.md
// section 4), so stdin is unbuffered: reading ahead would wait for input the program has not asked
// for yet. stderr also goes to stdout, so stdout is flushed before stderr is written.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct malm_file {
    int fd;
    int ungot; // byte pushed back by ungetc, or EOF
    bool eof;
    bool error;
    size_t len; // buffered output bytes
    size_t cap; // buffer size, 0 if unbuffered
    unsigned char* buf;
};

static unsigned char stdout_buf[BUFSIZ];

FILE malm_stdin = {.fd = 0, .ungot = EOF};
FILE malm_stdout = {.fd = 1, .ungot = EOF, .cap = BUFSIZ, .buf = stdout_buf};
FILE malm_stderr = {.fd = 2, .ungot = EOF};

static bool write_all(FILE* f, const unsigned char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(f->fd, p, n);
        if (w <= 0) {
            f->error = true;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool flush(FILE* f) {
    size_t n = f->len;
    f->len = 0;
    return write_all(f, f->buf, n);
}

// Called by exit (exit.c).
void __malm_stdio_flush(void) { flush(stdout); }

// Line buffering: the buffer is written when it is full and after a newline.
static bool put_bytes(FILE* f, const void* data, size_t n) {
    const unsigned char* p = data;
    if (f->cap == 0) {
        return flush(stdout) && write_all(f, p, n);
    }
    bool newline = false;
    for (size_t i = 0; i < n; ++i) {
        if (f->len == f->cap && !flush(f)) {
            return false;
        }
        f->buf[f->len++] = p[i];
        newline |= p[i] == '\n';
    }
    return !newline || flush(f);
}

int fputc(int c, FILE* f) {
    unsigned char b = (unsigned char)c;
    return put_bytes(f, &b, 1) ? b : EOF;
}

int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char* s, FILE* f) { return put_bytes(f, s, strlen(s)) ? 0 : EOF; }
int puts(const char* s) { return fputs(s, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF; }

size_t fwrite(const void* p, size_t size, size_t count, FILE* f) {
    if (size == 0 || count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / size) {
        f->error = true;
        return 0;
    }
    return put_bytes(f, p, size * count) ? count : 0;
}

int fflush(FILE* f) { return flush(f ? f : stdout) ? 0 : EOF; }

int fgetc(FILE* f) {
    if (f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }
    if (f->eof) {
        return EOF;
    }
    flush(stdout);
    unsigned char b;
    ssize_t r = read(f->fd, &b, 1);
    if (r == 1) {
        return b;
    }
    if (r == 0) {
        f->eof = true;
    } else {
        f->error = true;
    }
    return EOF;
}

int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
    if (c == EOF || f->ungot != EOF) {
        return EOF;
    }
    f->ungot = (unsigned char)c;
    f->eof = false;
    return f->ungot;
}

char* fgets(char* s, int n, FILE* f) {
    if (n <= 0) {
        return NULL;
    }
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) {
                return NULL;
            }
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    s[i] = '\0';
    return s;
}

size_t fread(void* p, size_t size, size_t count, FILE* f) {
    if (size == 0 || count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / size) {
        f->error = true;
        return 0;
    }
    unsigned char* dst = p;
    size_t want = size * count;
    size_t got = 0;
    if (f->ungot != EOF) {
        dst[got++] = (unsigned char)f->ungot;
        f->ungot = EOF;
    }
    if (got < want && !f->eof) {
        flush(stdout);
        ssize_t r = read(f->fd, dst + got, want - got);
        if (r < 0) {
            f->error = true;
        } else {
            got += (size_t)r;
            f->eof = got < want;
        }
    }
    return got / size;
}

int feof(FILE* f) { return f->eof; }
int ferror(FILE* f) { return f->error; }

void clearerr(FILE* f) {
    f->eof = false;
    f->error = false;
}
