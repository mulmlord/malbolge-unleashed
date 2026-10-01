# Build-time instrumentation of Olmstead's malbolge.c for the differential test.
#
#   cmake -DIN=<pristine malbolge.c> -DOUT=<instrumented copy> -P instrument.cmake
#
# The pristine file is never edited. This script applies the small textual transformations
# below and writes the result to OUT. Each snippet must occur exactly once in the input,
# otherwise this script aborts (FATAL_ERROR), so an upstream or accidental change cannot slip by
# silently. Everything else is the reference code, unchanged. Hook semantics: reference.h.
#
#   T1  `#include <string.h>`      + preamble: #include "reference.h", step counter, ref_op
#   T2  `int main(...)`            renamed ref_original_main (the file-based entry is not used)
#   T3  the `continue` line        + step cap; + hook and return instead of hanging when [C] is
#                                    outside 33..126 (spec 8: E_EXEC)
#   T4  `switch (xlat1[...])`      the decoded letter is kept in ref_op
#   T5  `putc( a, stdout )`        -> ref_putc( a )
#   T6  `x = getc( stdin );`       -> x = ref_getc();
#   T7  `case 'v': return;`        + hook before returning
#   T8  `mem[c] = xlat2[...]`      + hook and return when [C] is outside 33..126 (spec 8:
#                                    E_ENCRYPT). The reference indexes xlat2 out of bounds for
#                                    [C] < 33 and [C] > 127; for [C] == 127 it reads the string
#                                    terminator (xlat2[94]) and stores 0, which is defined
#                                    behaviour. Classic mode raises E_ENCRYPT for all of them.
#   T9  `if ( d == 59048 ) ...`    + per-step hook after C and D were incremented
#   T10 appended at the end        ref_run / ref_memory: entry point taking cells, not a file

if(NOT IN OR NOT OUT)
    message(FATAL_ERROR "instrument.cmake: pass -DIN=... -DOUT=...")
endif()

file(READ "${IN}" text)

# The file must be exactly the vendored one.
file(SHA256 "${IN}" got_hash)
set(want_hash "ca3b4f321bc3273195eb29eee7ee2002031b057c2bf0c8d7a4f7b6e5b3f648c0")
if(NOT got_hash STREQUAL want_hash)
    message(FATAL_ERROR "instrument.cmake: ${IN} has SHA-256 ${got_hash}, expected ${want_hash}")
endif()

# Replaces the one occurrence of `old` in `text`.
function(replace_once label old new)
    string(FIND "${text}" "${old}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "instrument.cmake: ${label}: snippet not found:\n${old}")
    endif()
    string(LENGTH "${old}" old_len)
    math(EXPR after "${first} + ${old_len}")
    string(SUBSTRING "${text}" ${after} -1 rest)
    string(FIND "${rest}" "${old}" again)
    if(NOT again EQUAL -1)
        message(FATAL_ERROR "instrument.cmake: ${label}: snippet occurs more than once:\n${old}")
    endif()
    string(SUBSTRING "${text}" 0 ${first} head)
    set(text "${head}${new}${rest}" PARENT_SCOPE)
endfunction()

replace_once(T1 [==[#include <string.h>
]==] [==[#include <string.h>

/* ---- instrumentation (instrument.cmake T1) ---- */
#include "reference.h"
static unsigned long ref_steps, ref_max;
static char ref_op;
/* ---- end ---- */
]==])

replace_once(T2 [==[int main( int argc, char **argv )]==]
                [==[int ref_original_main( int argc, char **argv )]==])

replace_once(T3 [==[    if ( mem[c] < 33 || mem[c] > 126 ) continue;
]==] [==[    if ( ref_steps++ >= ref_max ) return;
    if ( mem[c] < 33 || mem[c] > 126 )
    {
      ref_hook( REF_E_EXEC, 0, a, c, d, mem );
      return;
    }
]==])

replace_once(T4 [==[    switch ( xlat1[( mem[c] - 33 + c ) % 94] )
]==] [==[    ref_op = xlat1[( mem[c] - 33 + c ) % 94];
    switch ( ref_op )
]==])

replace_once(T5 [==[putc( a, stdout );]==] [==[ref_putc( a );]==])

replace_once(T6 [==[x = getc( stdin );]==] [==[x = ref_getc();]==])

replace_once(T7 [==[      case 'v': return;
]==] [==[      case 'v': ref_hook( REF_HALT, 'v', a, c, d, mem ); return;
]==])

replace_once(T8 [==[    mem[c] = xlat2[mem[c] - 33];
]==] [==[    if ( mem[c] < 33 || mem[c] > 126 )
    {
      ref_hook( REF_E_ENCRYPT, 0, a, c, d, mem );
      return;
    }
    mem[c] = xlat2[mem[c] - 33];
]==])

replace_once(T9 [==[    if ( d == 59048 ) d = 0; else d++;
]==] [==[    if ( d == 59048 ) d = 0; else d++;
    ref_hook( REF_STEP, ref_op, a, c, d, mem );
]==])

string(APPEND text [==[

/* ---- instrumentation (instrument.cmake T10) ---- */
static unsigned short ref_mem[59049];

void ref_run( const unsigned char *prog, unsigned long len, unsigned long max_steps )
{
  unsigned long i;
  for ( i = 0; i < len; i++ ) ref_mem[i] = prog[i];
  for ( ; i < 59049; i++ ) ref_mem[i] = op( ref_mem[i - 1], ref_mem[i - 2] );
  ref_steps = 0;
  ref_max = max_steps;
  exec( ref_mem );
}

const unsigned short *ref_memory( void )
{
  return ref_mem;
}
]==])

file(WRITE "${OUT}" "${text}")
