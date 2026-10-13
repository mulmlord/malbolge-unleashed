// Counts the lines, words and bytes of stdin and prints them as "lines words bytes", the POSIX
// format of `wc` without a file name. A word is a run of bytes that are not white space.
#include <ctype.h>
#include <stdio.h>

int main(void) {
    unsigned long lines = 0;
    unsigned long words = 0;
    unsigned long bytes = 0;
    int in_word = 0;
    unsigned char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) {
        bytes += n;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                ++lines;
            }
            if (isspace(buf[i])) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                ++words;
            }
        }
    }
    printf("%lu %lu %lu\n", lines, words, bytes);
    return 0;
}
