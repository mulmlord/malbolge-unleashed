# RISC-V guests

The contract between a RISC-V guest program and the machines that run it: the C++ reference
emulator (`src/rv/machine.hpp`, `malm rvref`), rv-emu (`rv/emu`, emu mode) and rv-aot
(`src/aot`). The reference emulator is the oracle; the other two must agree with it on stdout,
exit status and, for the differential tests, the final registers. Design background:
[the design spec](superpowers/specs/2026-10-01-malbolge-unleashed-design.md) sections 5.3, 5.4
and 6.

## 1. Programs

A guest is an RV32IM ELF executable: little-endian ELF32, `ET_EXEC`, `EM_RISCV`, built with
`clang --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32`. The loader (`src/rv/elf.hpp`,
shared by all three machines) rejects, with a one-line reason:

- anything else (not ELF, 64-bit, big-endian, not `ET_EXEC`, another machine);
- the `e_flags` bits for compressed instructions (`EF_RISCV_RVC`), a hardware-float ABI or RV32E;
- program headers or segment data past the end of the file, `p_filesz > p_memsz`;
- `PT_LOAD` segments outside guest RAM or overlapping each other, or none at all;
- an entry point that is not 4-aligned inside an executable (`PF_X`) segment.

Only `PT_LOAD` segments with `p_memsz > 0` are loaded, at `p_vaddr`; `p_paddr`, section headers
and other program headers are ignored. Bytes past `p_filesz` are zero.

## 2. Machine state

- **Guest RAM** is the byte range `[0, ram_size)` and the whole address space. `ram_size` is fixed
  per program (`--ram`, default 1 MiB = `1M`): a multiple of 16 up to 1 GiB. RAM is zero except
  for the loaded segments. Code and data share it, without permissions: stores into code are
  seen by later fetches.
- **Registers** at the start: `pc` = the ELF entry, `sp` (x2) = `ram_size`, all others 0.
- **Program break** (`brk`) starts at the end of the highest segment (`p_vaddr + p_memsz`).

## 3. Instructions

RV32I and the M extension, as in the unprivileged ISA specification (division by zero and signed
overflow give the defined results). In addition:

- `fence` and `fence.i` are no-ops (one hart; every fetch reads RAM).
- Loads and stores may be misaligned; they access the bytes little-endian as usual.
- `ecall` is a system call (section 4).

Everything else faults (section 5): any encoding outside RV32IM (including compressed
instructions, CSR instructions, `ebreak`, reserved `funct3`/`funct7` values and `slli`/`srli`/`srai`
with `imm[5]` set), a taken jump or branch to an address that is not 4-aligned, a load, store or
fetch that is not entirely inside guest RAM.

## 4. System calls

`ecall` takes the number in `a7` and arguments in `a0`..`a2`, returns in `a0`, and changes no
other register. The numbers are those of Linux RV32. Errors are negated Linux errno values.

| `a7` | call | behaviour |
|---|---|---|
| 63 | `read(fd, buf, count)` | `fd` must be 0. Reads stdin one byte at a time into `buf` until `count` bytes or end of input; returns the number read, 0 at end of input. It never returns early with fewer bytes while input remains, so the result does not depend on how the host delivers input (rv-emu runs one Malbolge `In` per byte). Output is flushed first. |
| 64 | `write(fd, buf, count)` | `fd` must be 1 or 2; both go to stdout. Writes the bytes, returns `count`. |
| 93 | `exit(code)` | ends the program with exit status `code & 255`. |
| 214 | `brk(addr)` | if `initial break <= addr <= ram_size`, the break becomes `addr`; returns the break after the call (so `brk(0)` reads it). |

`read` from another fd and `write` to another fd return `-EBADF` (-9); a buffer that is not
entirely inside guest RAM gives `-EFAULT` (-14), with no I/O. Any other number returns `-ENOSYS`
(-38).

## 5. Faults

A fault stops the program with exit status 1 and a diagnostic on stderr; `pc` still names the
faulting instruction. Host I/O errors (stdout closed, a broken pipe, a read error) are faults
too, reported as `E_IO`. A guest that loops forever runs forever.

The reference emulator reports `malm rvref: <reason> at pc=0x... (step S)`, where `S` counts
instructions from 1, the faulting one included. Reasons:

- `illegal instruction 0x...` (the instruction word)
- `ebreak`
- `misaligned jump target 0x...`
- `load from 0x... outside guest RAM`, `store to 0x... outside guest RAM`
- `instruction fetch outside guest RAM`
- `E_IO: cannot write to stdout`, `E_IO: cannot read from stdin`

## 6. Runtime

`rv/runtime` is what a C program needs to run as a guest: the entry point (`crt0.S`), the linker
script (`link.ld`), the system call wrappers and a minimal C library (`include/`, `src/`).

### Toolchain and linking

The runtime needs clang with the riscv32 target and `ld.lld` (Arch: `clang lld`; Ubuntu:
`clang-18 lld-18`), nothing else: no GCC toolchain, sysroot, compiler-rt or `llvm-ar`. Until
`malm build` does it, a program is built like this (the CMake build in `tests/rv` does the same):

```sh
target="--target=riscv32-unknown-elf -march=rv32im -mabi=ilp32"
inc="-nostdlibinc -isystem rv/runtime/include -ffunction-sections -fdata-sections"
clang $target -c rv/runtime/crt0.S -o crt0.o
for f in rv/runtime/src/*.c; do clang $target $inc -O2 -ffreestanding -c "$f"; done  # the libc
clang $target $inc -O2 -c prog.c -o prog.o
ld.lld -T rv/runtime/link.ld --gc-sections crt0.o prog.o \
    --start-lib assert.o ctype.o divdi3.o exit.o malloc.o printf.o stdio.o stdlib.o string.o \
    syscalls.o --end-lib -o prog.elf
malm rvref prog.elf
```

- `-nostdlibinc` drops the host's headers but keeps clang's own freestanding ones (`stddef.h`,
  `stdint.h`, `stdarg.h`, `stdbool.h`, `limits.h`, ...), which complete the runtime's.
- The library is compiled with `-ffreestanding`, so clang does not turn its loops into calls to the
  functions they implement. Programs are compiled hosted: clang may replace `printf("hi\n")` with
  `puts("hi")` and the like, which the library provides.
- `--start-lib ... --end-lib` gives the library objects archive semantics without `llvm-ar`: an
  object is linked only if it defines a symbol the program needs; `--gc-sections` then drops the
  unused functions. A hello world is about 1 KB of code.
- Floating-point arithmetic needs soft-float routines (`__addsf3`, ...) that the runtime does not
  have: such a program fails to link.

### Memory layout

| addresses | contents |
|---|---|
| `[0, 0x1000)` | unused; nothing faults there, a null pointer reads zeros |
| `0x1000` up | `.text` (`_start` first), `.rodata`, `.init_array`, `.data`, `.bss`, up to `_end` |
| `_end` up | the heap, grown through `brk` |
| below `ram_size` | the stack, growing down from the initial `sp` |

`sbrk` and `brk` keep the heap at least `__stack_size` bytes below the initial `sp` (default 64 KiB,
`ld.lld --defsym=__stack_size=N` to change it) and fail with `ENOMEM` beyond that, so `malloc`
returns NULL instead of handing out the stack. Nothing stops a stack deeper than `__stack_size`
from running into the heap.

### crt0

`_start` clears `.bss` (the loaders already zero it; this keeps the runtime independent of that),
aligns `sp` to 16 and records it for the heap limit, runs the `.init_array` constructors, and calls
`main(0, argv, envp)` with `argv` and `envp` pointing to empty (NULL-terminated) vectors, then
`exit` with main's result. The exit status is that value `& 255`.

### Library

| header | contents |
|---|---|
| `unistd.h` | `read`, `write`, `_exit`, `brk`, `sbrk`: the system calls of section 4; an error sets `errno` and returns -1 |
| `errno.h` | `errno` and the Linux error numbers |
| `stdlib.h` | `malloc`, `calloc`, `realloc`, `free` (first-fit, address-ordered free list with merging, on `sbrk`; blocks 16-aligned); `exit`, `atexit` (32 functions), `abort`; `abs`, `labs`, `atoi`, `atol`, `strtol`, `strtoul`; `qsort` (heapsort) |
| `string.h` | `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`, `strstr`, `strdup` |
| `stdio.h` | `stdin`, `stdout`, `stderr`; `fputc`, `putc`, `putchar`, `fputs`, `puts`, `fwrite`, `fflush`; `fgetc`, `getc`, `getchar`, `ungetc` (one byte), `fgets`, `fread`; `feof`, `ferror`, `clearerr`; `printf`, `fprintf`, `sprintf`, `snprintf` and their `v` forms |
| `ctype.h` | the `is...` classes and `tolower`, `toupper` of the "C" locale (ASCII) |
| `assert.h` | `assert` (a failure prints ``file:line: function: Assertion `expr' failed.`` on stderr and aborts), `static_assert` |
| (none) | `__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`: the 64-bit division clang calls for `long long` |

- **Streams.** `stdout` is line-buffered (1024 bytes): it is written when the buffer is full, after
  a newline, before a read from `stdin`, before a write to `stderr` (which also goes to stdout, so
  the two stay in order) and by `exit`. `_exit`, `abort` and a fault lose what is buffered.
  `stdin` is unbuffered: `read` blocks until it has every byte asked for (section 4), so reading
  ahead would wait for input the program has not asked for yet. `getchar` reads one byte per
  `read`, `fread` passes its whole request to one `read`.
- **printf** knows the conversions `d i u o x X c s p %`, the flags `- + space # 0`, width and
  precision (also `*`) and the length modifiers `hh h l ll j z t`. `%p` prints like `%#x`. A
  floating-point conversion (`f e g a` in either case) is printed as written, `%f` for example, and
  its argument skipped.
- **exit** runs the `atexit` functions in reverse order, flushes `stdout` and ends the program with
  `ecall` 93. `abort` ends it at once with status 134 (128 + SIGABRT, what a shell reports for a
  real abort).

Everything else in the C library (`scanf`, string to floating-point conversion, files, time,
locales, wide characters, `setjmp`, signals) is missing; picolibc is planned for that.

## 7. Tests

`tests/rv/`: unit tests for the loader and every instruction class (`rv_test`), the official
[riscv-tests](https://github.com/riscv-software-src/riscv-tests) `rv32ui-p-*` and `rv32um-p-*`
on `malm rvref`, and C programs through real pipes with their output and exit status checked:
`tests/rv/programs/hello.c` and `cat.c` without the runtime (for the initial and final machine
state), and on the runtime the examples in `examples/c` (hello, cat, fizzbuzz) and
`tests/rv/programs/runtime_test.c`, a self-check of crt0 and the library.

riscv-tests is fetched at configure time (pinned commit) and built with clang and ld.lld, without
its `env` submodule: `tests/rv/env/riscv_test.h` replaces the machine-mode environment with one
that needs no CSRs or trap handler. A test starts at `_start` and exits through `ecall` 93 with
status 0 on pass and the failing test number (mod 256, never 0) on fail. Every test ELF is linked
with `rv/runtime/link.ld`.

The CMake cache variable `MALM_RV_TESTS` controls these toolchain-built tests: `AUTO` (default)
builds them when clang with the riscv32 target and ld.lld are found and otherwise skips them with
a warning, `ON` makes a missing toolchain a configure error (CI), `OFF` skips them. The
toolchain is found as `MALM_RV_CC` and `MALM_RV_LD`, which can also be set by hand.
