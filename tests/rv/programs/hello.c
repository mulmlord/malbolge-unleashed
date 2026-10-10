// Hello world for the reference emulator: one write to stdout, exit status 0.
#include "sys.h"

void _start(void) {
    static const char msg[] = "Hello, world!\n";
    sys_write(1, msg, sizeof msg - 1);
    sys_exit(0);
}
