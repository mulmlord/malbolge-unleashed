// FizzBuzz from 1 to the number read from stdin. Input that is not a number from 1 to 10000 gives
// a message on stderr and exit status 2.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char line[32];
    char* end;
    long n = fgets(line, sizeof line, stdin) ? strtol(line, &end, 10) : 0;
    if (n < 1 || n > 10000 || end == line || (*end != '\0' && *end != '\n')) {
        fputs("fizzbuzz: expected a number from 1 to 10000\n", stderr);
        return 2;
    }
    for (long i = 1; i <= n; ++i) {
        if (i % 15 == 0) {
            puts("FizzBuzz");
        } else if (i % 3 == 0) {
            puts("Fizz");
        } else if (i % 5 == 0) {
            puts("Buzz");
        } else {
            printf("%ld\n", i);
        }
    }
    return 0;
}
