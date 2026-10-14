// Reads numbers from stdin and prints their count, minimum, maximum, mean and standard deviation.
// Needs picolibc (malm build --libc picolibc) for floating point, scanf and sqrt.
#include <math.h>
#include <stdio.h>

int main(void) {
    int n = 0;
    double x, min = 0, max = 0, mean = 0, m2 = 0;
    int r;
    while ((r = scanf("%lf", &x)) == 1) {
        // Welford's running mean and sum of squared deviations.
        ++n;
        double d = x - mean;
        mean += d / n;
        m2 += d * (x - mean);
        if (n == 1 || x < min) {
            min = x;
        }
        if (n == 1 || x > max) {
            max = x;
        }
    }
    if (r != EOF) {
        fputs("stats: expected numbers\n", stderr);
        return 2;
    }
    printf("count %d\n", n);
    if (n > 0) {
        printf("min %g\nmax %g\nmean %g\nstddev %g\n", min, max, mean, sqrt(m2 / n));
    }
    return 0;
}
