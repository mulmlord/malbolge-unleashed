// Streams on fds 0, 1 and 2. stdout is line-buffered and flushed before stdin is read, stderr is
// written or exit; stderr and stdin are unbuffered. printf handles d i u o x X c s p % with flags,
// width, precision and the length modifiers hh h l ll j z t; no floating point.
#pragma once

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)
#define BUFSIZ 1024

typedef struct malm_file FILE;

extern struct malm_file malm_stdin, malm_stdout, malm_stderr;
#define stdin (&malm_stdin)
#define stdout (&malm_stdout)
#define stderr (&malm_stderr)

int fputc(int c, FILE* f);
int putc(int c, FILE* f);
int putchar(int c);
int fputs(const char* s, FILE* f);
int puts(const char* s);
size_t fwrite(const void* p, size_t size, size_t count, FILE* f);
int fflush(FILE* f);

int fgetc(FILE* f);
int getc(FILE* f);
int getchar(void);
int ungetc(int c, FILE* f);
char* fgets(char* s, int n, FILE* f);
size_t fread(void* p, size_t size, size_t count, FILE* f);

int feof(FILE* f);
int ferror(FILE* f);
void clearerr(FILE* f);

__attribute__((format(printf, 1, 2))) int printf(const char* fmt, ...);
__attribute__((format(printf, 2, 3))) int fprintf(FILE* f, const char* fmt, ...);
__attribute__((format(printf, 2, 3))) int sprintf(char* s, const char* fmt, ...);
__attribute__((format(printf, 3, 4))) int snprintf(char* s, size_t n, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vsprintf(char* s, const char* fmt, va_list ap);
int vsnprintf(char* s, size_t n, const char* fmt, va_list ap);
