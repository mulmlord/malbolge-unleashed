// stdin, stdout and stderr for picolibc's stdio (docs/rv.md section 6), with the minimal runtime's
// buffering (src/stdio.c). read blocks until it has all the bytes asked for (docs/rv.md section 4),
// so stdin reads one byte at a time: reading ahead would wait for input the program has not asked
// for yet. stdout is line-buffered. stderr also goes to stdout, so stdout is flushed before each
// byte of stderr is written.
#include <stdio.h>
#include <unistd.h>

static char out_buf[1024];
static size_t out_len;

static int flush_out(FILE* f) {
    (void)f;
    const char* p = out_buf;
    size_t n = out_len;
    out_len = 0;
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w <= 0) {
            return EOF;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// Written when the buffer is full and after a newline.
static int put_out(char c, FILE* f) {
    out_buf[out_len++] = c;
    if ((c == '\n' || out_len == sizeof out_buf) && flush_out(f) != 0) {
        return _FDEV_ERR;
    }
    return (unsigned char)c;
}

static int put_err(char c, FILE* f) {
    (void)f;
    if (flush_out(stdout) != 0 || write(STDERR_FILENO, &c, 1) != 1) {
        return _FDEV_ERR;
    }
    return (unsigned char)c;
}

static int get_in(FILE* f) {
    (void)f;
    flush_out(stdout);
    unsigned char b;
    ssize_t r = read(STDIN_FILENO, &b, 1);
    return r == 1 ? b : r == 0 ? _FDEV_EOF : _FDEV_ERR;
}

static FILE in = FDEV_SETUP_STREAM(NULL, get_in, NULL, _FDEV_SETUP_READ);
static FILE out = FDEV_SETUP_STREAM(put_out, NULL, flush_out, _FDEV_SETUP_WRITE);
static FILE err = FDEV_SETUP_STREAM(put_err, NULL, NULL, _FDEV_SETUP_WRITE);

FILE* const stdin = &in;
FILE* const stdout = &out;
FILE* const stderr = &err;

// exit runs the destructors last to first (link.ld): the atexit functions, then the program's
// destructors, then this one, which has the lowest priority number.
__attribute__((destructor(101))) static void flush_at_exit(void) { flush_out(stdout); }
