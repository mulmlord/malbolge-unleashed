// The rest of the POSIX interface picolibc builds on, for a machine with stdin and stdout and
// nothing else (docs/rv.md section 6): there are no files and no clock, so fopen, remove, time and
// clock link and fail at run time. stdin and stdout are pipes. Every signal ends the program like
// an uncaught signal in a shell, with status 128 + sig (abort: 134, as in the minimal runtime).
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

static int fail(int error) {
    errno = error;
    return -1;
}

static int is_std(int fd) { return fd >= 0 && fd <= STDERR_FILENO; }

int open(const char* path, int flags, ...) {
    (void)path;
    (void)flags;
    return fail(ENOSYS);
}

int close(int fd) { return is_std(fd) ? 0 : fail(EBADF); }

off_t lseek(int fd, off_t offset, int whence) {
    (void)offset;
    (void)whence;
    return fail(is_std(fd) ? ESPIPE : EBADF);
}

int isatty(int fd) {
    errno = is_std(fd) ? ENOTTY : EBADF;
    return 0;
}

int unlink(const char* path) {
    (void)path;
    return fail(ENOSYS);
}

int gettimeofday(struct timeval* restrict tv, void* restrict tz) {
    (void)tv;
    (void)tz;
    return fail(ENOSYS);
}

clock_t times(struct tms* buf) {
    (void)buf;
    errno = ENOSYS;
    return (clock_t)-1;
}

int raise(int sig) { _exit(128 + sig); }
