#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// Blocks are 16-aligned (max_align_t); the heap grows through sbrk.
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t size);
void free(void* p);

// exit runs the atexit functions (at most 32) in reverse order and flushes stdout; abort does
// neither and exits with status 134 (128 + SIGABRT, what a shell reports for a real abort).
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void abort(void);
int atexit(void (*fn)(void));

int abs(int x);
long labs(long x);
int atoi(const char* s);
long atol(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);

void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*));
