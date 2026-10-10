// cat through a buffer on the heap (brk), so it exercises read, write, brk and exit. The exit status
// is the number of bytes copied mod 256.
#include "sys.h"

enum { kBufSize = 4096 };

void _start(void) {
    char* buf = sys_brk(0);
    if (sys_brk(buf + kBufSize) != buf + kBufSize) {
        sys_exit(255);
    }
    unsigned long total = 0;
    long n;
    while ((n = sys_read(0, buf, kBufSize)) > 0) {
        if (sys_write(1, buf, (unsigned long)n) != n) {
            sys_exit(254);
        }
        total += (unsigned long)n;
    }
    sys_exit((int)(total & 255));
}
