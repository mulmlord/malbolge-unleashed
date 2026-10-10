#include <stdlib.h>
#include <unistd.h>

// Defined by stdio.c; weak, so a program without stdio does not link it in.
__attribute__((weak)) void __malm_stdio_flush(void);

enum { kMaxAtexit = 32 };
static void (*handlers[kMaxAtexit])(void);
static int handler_count;

int atexit(void (*fn)(void)) {
    if (handler_count == kMaxAtexit) {
        return -1;
    }
    handlers[handler_count++] = fn;
    return 0;
}

void exit(int status) {
    while (handler_count > 0) {
        handlers[--handler_count]();
    }
    if (__malm_stdio_flush) {
        __malm_stdio_flush();
    }
    _exit(status);
}

void abort(void) { _exit(134); }
