/* Interface between the instrumented copy of Olmstead's malbolge.c (see instrument.cmake) and the
 * differential test. C and C++ compatible. The hooks are implemented by the test, the other
 * functions by the instrumented reference. */
#ifndef MALM_REFERENCE_H
#define MALM_REFERENCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Kinds of per-step events reported by ref_hook. */
enum {
    REF_STEP = 0,      /* a step finished; a, c, d are after the increment of C and D */
    REF_HALT = 1,      /* 'v' executed; a, c, d unchanged */
    REF_E_EXEC = 2,    /* [C] outside 33..126 (the reference would hang); registers of the step start */
    REF_E_ENCRYPT = 3  /* [C] outside 33..126 before xlat2: the reference indexes out of bounds for
                          [C] < 33 and [C] > 127, and for [C] == 127 reads the string terminator and
                          stores 0; classic mode raises E_ENCRYPT for all of them. a is updated, c
                          and d are not incremented */
};

/* Called by the reference after every step. `op` is the decoded xlat1 letter (a char) for REF_STEP
 * and REF_HALT, 0 for the faults. `mem` is the reference's memory (59049 cells). */
void ref_hook(int kind, int op, unsigned a, unsigned c, unsigned d, const unsigned short* mem);
/* Replace getc(stdin) / putc(a, stdout). ref_getc returns 0..255 or -1 (EOF). */
int ref_getc(void);
void ref_putc(int c);

/* Loads `len` >= 2 cells (each in 33..126), fills the rest of memory with op() and runs exec() for
 * at most `max_steps` steps. Stops after a hook of kind != REF_STEP, or silently at the step cap. */
void ref_run(const unsigned char* prog, unsigned long len, unsigned long max_steps);
/* The 59049 cells after ref_run. */
const unsigned short* ref_memory(void);

#ifdef __cplusplus
}
#endif

#endif
