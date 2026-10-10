// System call wrappers (docs/rv.md section 4). On error they set errno and return -1.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int ssize_t;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Blocks until count bytes or end of input: a short count means end of input.
ssize_t read(int fd, void* buf, size_t count);
// fd 1 and 2 both go to stdout.
ssize_t write(int fd, const void* buf, size_t count);
__attribute__((noreturn)) void _exit(int status);

// The heap ends __stack_size bytes (link.ld) below the initial stack pointer.
int brk(void* addr);
void* sbrk(intptr_t increment);
