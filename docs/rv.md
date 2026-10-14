# RISC-V guests

The contract between a RISC-V guest program and the machines that run it: the C++ reference
emulator (`src/rv/machine.hpp`, `malm rvref`), rv-emu (section 8, emu mode) and rv-aot
(section 9, aot mode). The reference emulator is the oracle; the other two must agree with it on stdout,
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
| 255 | register dump | not Linux: writes `== registers ==`, then `pc 0x...` (the `ecall`) and `x1 0x...` .. `x31 0x...`, one per line with 8 lowercase hex digits, to stdout; changes no register. For the differential tests (section 7). |

`read` from another fd and `write` to another fd return `-EBADF` (-9); a buffer that is not
entirely inside guest RAM gives `-EFAULT` (-14), with no I/O. Any other number returns `-ENOSYS`
(-38). All three machines implement the register dump the same way, so their stdout can be
compared byte for byte. In rv-emu and rv-aot it is a subroutine of the runtime that computes each
hex digit (no table); it adds about 2.3 M characters of Malbolge text, and as many startup steps,
to every program, most of it the 32 register reads (each register access costs program text, hla.md
4).

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
`clang-18 lld-18`), nothing else: no GCC toolchain, sysroot, compiler-rt or `llvm-ar`.
`malm build prog.c -o prog.malm` ([cli.md](cli.md)) compiles and links a C program with the
runtime, which is built into `malm`, and builds it for emu mode (section 8). By hand, the same ELF
is built like this (the CMake build in `tests/rv` does the same):

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
| `unistd.h` | `read`, `write`, `_exit`, `brk`, `sbrk`: the system calls of section 4; an error sets `errno` and returns -1. A program linked with `--defsym=__malm_dump_regs=1` dumps its registers (ecall 255) in `_exit`, so on every way out (`exit`, a return from `main`, `abort`) |
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
state), and on the runtime the examples in `examples/c` (hello, cat, wc, fizzbuzz, sort) and
`tests/rv/programs/runtime_test.c`, a self-check of crt0 and the library.
`rv_toolchain_test` compiles C with `malm build`'s toolchain code and the runtime built into
`malm` and runs the result on the reference emulator; the `build_c_*` and `build_c_aot_*` tests
(`tests/rv/build_pipe_test.sh`) build each example with `malm build` in emu and aot mode, as a
`.malm` and with `--exe`, and run both through real pipes (`cat` is `fast`, the rest `slow`; they
need `MALM_BUILD_EMU`).
`rv_emu_test` runs rv-emu against the reference emulator one instruction at a time (section 8).
`rv_aot_test` checks rv-aot's control-flow graph and runs translated hand-assembled programs
(random ALU code; loads, stores, branches, calls, a jump table and the system calls; every fault
and jump out of the code) against the reference emulator, comparing output, exit status, stop
code, registers and RAM, and reports the cost of each instruction class (section 9).
`tests/rv/aot_bench.sh` is not a test but the aot-mode benchmark
([aot-benchmarks.md](aot-benchmarks.md)): the sizes and VM steps of the differential test's programs.
`rv_test` also builds a hand-assembled ELF with `malm build` in both modes and runs it, without
the RISC-V toolchain.

**The three-way differential test** (`tests/rv/rv_diff_test.sh`, the M4 exit criterion) runs a
program on the reference emulator (`malm rvref`) and builds it in emu and aot mode (`malm build`,
then `malm run`), each input through real pipes, and requires the same stdout, byte for byte, and
the same exit status from all three. The programs dump their registers as they exit (ecall 255,
section 4), so stdout ends with the final `pc` and registers: the riscv-tests through
`env/riscv_test.h` before the pass or fail `exit`, the C programs through the runtime's `_exit`
(linked with `--defsym=__malm_dump_regs=1`; the bare `hello.c` and `cat.c` with
`-DMALM_DUMP_REGS`, see `programs/sys.h`). The script checks that the dump is there, and, for the
riscv-tests, that the reference emulator passes (status 0).

- `rvdiff_rv32u*-p-*`: every riscv-test with 8 KiB of guest RAM (each ends below `0x2000` and uses
  no stack), about 8 seconds per test in a release build. `fence_i` modifies its own code, so aot
  mode must refuse it (the build error of section 9) and only emu mode is compared. `add`, `jalr`,
  `lw`, `ma_data` and `fence_i` are `fast`, the rest `slow`.
- `rvdiff_program_*`: the e2e suite with 64 KiB of guest RAM: the bare `hello.c` and `cat.c`, and on
  the runtime the examples hello, cat, wc, fizzbuzz (also its error path, status 2) and sort (with a
  16 KiB heap reserve, as 64 KiB leaves none by default), each with the inputs in `tests/rv/inputs`;
  and the runtime's self-check with 256 KiB, on both of its inputs (exit 42, and a failed `assert`,
  134). The example hello is `fast`, the rest `slow`; the self-check takes about 11 minutes, nearly
  all of it in emu mode.

They need `MALM_BUILD_EMU`, so the debug preset leaves them out. The `build_c_*` and
`build_c_aot_*` tests check the same examples against fixed expected output, built from C by
`malm build` as a `.malm` and with `--exe`.

riscv-tests is fetched at configure time (pinned commit) and built with clang and ld.lld, without
its `env` submodule: `tests/rv/env/riscv_test.h` replaces the machine-mode environment with one
that needs no CSRs or trap handler. A test starts at `_start` and exits through `ecall` 93 with
status 0 on pass and the failing test number (mod 256, never 0) on fail. Every test ELF is linked
with `rv/runtime/link.ld`.

The CMake cache variable `MALM_RV_TESTS` controls these toolchain-built tests: `AUTO` (default)
builds them when clang with the riscv32 target and ld.lld are found and otherwise skips them with
a warning, `ON` makes a missing toolchain a configure error (CI), `OFF` skips them. The
toolchain is found as `MALM_RV_CC` and `MALM_RV_LD`, which can also be set by hand.

## 8. rv-emu

rv-emu is the RV32IM emulator written in HLA ([hla.md](hla.md)), the core of emu mode. It is
generated rather than written by hand: `src/rv/emu.cpp` writes its HLA program (`malm rvemu`,
[cli.md](cli.md)), and the build compiles that with `malm hla` into `emu.malm`
(`rv/emu/CMakeLists.txt`; guest RAM `MALM_EMU_RAM`, default `1M`; `MALM_BUILD_EMU=OFF` skips it,
as the debug preset does). That `emu.malm` has no guest program; `malm build` makes one that has.

### Building a program

`malm build prog.elf -o prog.malm` ([cli.md](cli.md); `build_emu` in `src/driver/build_cmd.hpp`)
is the emu-mode image builder: one pure `.malm` (spec 4) with the `EXIT_A` flag that runs the
program in rv-emu. It loads the ELF like the reference emulator (section 1) and writes the
emulator's HLA program with the guest's initial state as initial values: `pc` the entry, `x2` the
RAM size, `brk` and `brkmin` the end of the image, and RAM the segments' bytes as little-endian
words. HLA's lowering and the assembler's startup code (asm.md 7) turn them into straight-line code
that writes guest RAM and the registers before the emulator's first `fetch`, so nothing is
preloaded. The result passes the purity check of `malm hla`.

The guest RAM size (`--ram`, default `1M`, at most `4M`) is fixed per program; `n` is HLA's 38 for
every size. The builder compiles the whole emulator for each program rather than adding to a
prebuilt `emu.malm`: the startup code is the entire program text and sets the layout's base, and it
writes every word of guest RAM anyway (each a record of its own, ADR 0001), so a guest image only
changes the values written. Building takes about 25 seconds with 1 MiB of guest RAM and 4 with
64 KiB (release build), about 70% of it generating the startup code.

### State

| guest | HLA |
|---|---|
| `x1` .. `x31` | registers `x1` .. `x31` (`x2` starts at `ram_size`); `x0` is the constant 0 |
| `pc` | register `pc` (the ELF entry) |
| guest RAM `[0, ram_size)` | the RAM region `mem` at HLA address 0, so a guest address is an HLA byte address; at most 4 MiB (HLA's limit of 2^20 words) |
| program break | registers `brk` and `brkmin` (the end of the image) |
| why it stopped | register `stop` (`src/rv/semantics.hpp` `StopCode`: 0 running, 1 illegal instruction, 2 `ebreak`, 3 misaligned jump, 4 and 5 a load or store outside RAM, 6 a fetch outside RAM, 7 `exit`; 8 only in aot mode, section 9) |

### The loop

Each turn starts at the label `fetch`: `load insn, [pc]`, then a `switch` on the low byte of the
instruction (the opcode; for branches bit 7 is bit 11 of the offset, so the switch also decodes
it). An instruction reads its operands through two subroutines (`rs1`, `rs2`: a `switch` on the
register field into 32 `mov`s), decodes `funct3` and `funct7` with `switch`es, builds its
immediate with shifts and masks, runs its semantics, and writes `rd` through `wb` (another
`switch`, `x0` skipped). `next` advances `pc`: inside a 256-byte block it flips the bits of the
low byte with an `xor` (150 steps instead of the 1 750 of an `add`); the last word of a block, and
the last word of RAM, take the `add` and the range check.

The semantics are C++ functions that write HLA (`src/rv/semantics.hpp`), shared with rv-aot: an
ALU instruction is one HLA statement (HLA's arithmetic is RV32IM's); branches are `xor`, `slt` or
`sltu` and `bz`/`bnz`; loads and stores are subroutines that check the address, `switch` on its
low two bits and assemble or merge bytes from one or two aligned HLA words (misaligned accesses
work, as in section 3); `ecall` dispatches on `a7` with a `switch` (and checks its upper bits):
`read` runs HLA's `in` (Malbolge `In`) once per byte, `write` `out` per byte, `exit` halts with `a0
& 255` (`EXIT_A`), `brk` moves the break. Subroutines are entered by `mov link, L; jmp
subroutine` and return by `jmp link` (60 steps, against 330 for HLA's `call` and `ret`).

Faults (section 5) set `stop` and halt with exit status 1. Unlike the reference emulator, rv-emu
has no stderr: the diagnostic is the register `stop` and `pc`, which names the faulting
instruction (for a fetch fault, the address outside RAM), as in the reference emulator.
A jump or branch to an aligned address outside RAM faults when `pc` is set rather than at the
next fetch; the registers are the same either way.

### Costs

VM steps per guest instruction, from `rv_emu_test` (means over its cases; every instruction
includes the fetch, the opcode switch and `next`, about 1 100 steps):

| instruction | steps |
|---|---|
| `lui` / `auipc` | 2 200 / 4 250 |
| `addi`, `andi`, `ori`, `xori` | 5 500 to 7 300 |
| `add`, `sub`, `slt`, `sltu` (register) | 8 500 |
| `and`, `or`, `xor` / shifts (register) | 6 800 / 7 500 to 8 300 |
| branch not taken / taken | 5 300 to 7 800 / 11 400 to 14 000 |
| `jal` / `jalr` | 11 600 / 10 800 |
| `lw` / `lb`, `lbu`, `lh`, `lhu` (aligned) | 9 400 / 10 000 to 10 700 |
| `sw` / `sb`, `sh` (aligned) | 10 800 / 12 400 to 12 700 |
| misaligned `lh`, `lw` / `sh`, `sw` | 12 000 to 15 000 / 16 400 to 17 900 |
| `mul` / `mulh`, `mulhsu`, `mulhu` | 50 000 / 57 500 |
| `div`, `divu`, `rem`, `remu` | 125 000 to 130 000 |
| `ecall` (`brk`, a few bytes of `read`/`write`) | 17 000 |

`read` and `write` add about 5 500 steps per byte. The ALU instructions with a register operand
(`add`: 2 000) and the operand reads (`rs1`, `rs2`: about 1 100 each, 640 of them the shift that
extracts the field) dominate; loads and stores add the range check (`and`, `bnz`: 1 300) and the address
`add`.

Instructions per second, measured with `malm run --stats` (release build, one core of a desktop
machine that was also running other programs, so the times are rough; the VM reaches about 40 M
steps per second on that machine when it is idle; issue #37 will add proper benchmarks):

| program, guest RAM | guest instructions | VM steps | time | instructions/s |
|---|---|---|---|---|
| `runtime_test.c` (the runtime's self-check), 1 MiB | 3 120 348 | 27.3 G (8 740 per instruction) | 1 224 s (22.3 M steps/s) | 2 550 |
| `hello.c` (bare), 64 KiB | 11 | 36.6 M, nearly all startup | 0.9 s (40 M steps/s) | |

A large guest RAM costs memory rather than speed. Every RAM word is a record in a VM page of its
own group (ADR 0001, "VM memory"), so 1 MiB of guest RAM touches about 100 000 VM pages, about
5 GB of host memory; the VM runs about 15% slower than with 64 KiB (measured back to back).
Smaller pages are VM work (issue #37).

Program size and startup (the startup code writes the registers, RAM and tables, spec 4):

| guest RAM | `emu.malm` | Malbolge text and startup steps | startup time |
|---|---|---|---|
| 4 KiB | 0.60 MB | 20 M | under a second |
| 64 KiB | 0.72 MB | 30 M | about a second |
| 1 MiB (the default) | 2.3 MB | 221 M | 6 to 60 seconds (idle to busy machine) |

The 26 `switch` tables (320 K characters each), the arithmetic library and the register dump
(section 4) make up the 20 M of the smallest; each word of guest RAM adds about 750 characters. `malm hla` compiles the 1 MiB
emulator in about 25 seconds (release build). HLA's outlining (hla.md 4.8, issue #31) took the
emulator from 28 M to 20 M characters and its runs about 5% fewer steps.

## 9. rv-aot

rv-aot is aot mode: `malm build --mode aot` ([cli.md](cli.md)) translates the program into HLA ahead
of time, every guest instruction into inline HLA, and compiles that into one pure `.malm` with the
`EXIT_A` flag. `src/aot/cfg.cpp` recovers the control-flow graph, `src/aot/aot.cpp` writes the HLA
program (`malm rvaot` prints it). It reuses rv-emu's state, instruction semantics and runtime
(`src/rv/semantics.hpp`: the ALU statements, branches, the load, store and system call subroutines
and the stops), so the two modes agree by construction where the semantics are concerned; what aot
saves is the decoding, the operand `switch`es and `pc` arithmetic, which happen at build time.

### The control-flow graph

- **The code** is every 4-aligned word inside an executable (`PF_X`) segment, decoded at build
  time. Nothing else is translated: with the runtime's linker script `.text` has a segment of its
  own, `.rodata` and `.data` others.
- **Basic blocks** start at the entry, the targets of branches and `jal`, the instruction after a
  branch or jump, and the indirect targets. Each starts with the HLA label `b_<address>`, in
  address order, so a block falls through into the next.
- **Indirect targets**, the entries of the dispatch table: the entry, the return addresses (after a
  `jal` or `jalr` with `rd` not `x0`), every aligned word of the image whose value is a code address
  (jump tables, function pointers, `.init_array`; an instruction word never is one, as its low two
  bits are 11), and the code addresses that `lui`, `auipc` and `addi` build from constants inside a
  block (`la a0, f`).
- The same constant tracking turns a `jalr` on a constant (`auipc` + `jalr`, a far `call` or
  `tail`) into a direct jump.

### Translation

The guest registers are the HLA registers `x1` .. `x31`, guest RAM the region `mem` at HLA address
0, with the program break and `stop` as in section 8. `x0` is a constant: an instruction with
`rd = x0` is dropped (a load still runs, as it can fault), and an operand `x0` is a constant or the
register `zero`, which is never written. `pc` is written only where something reads it: before a
load, a store or an `ecall` (the runtime's faults and `exit`) and in the stops.

**Constants.** Inside a block aot knows the registers that hold constants: `x0`, and what `lui`,
`auipc` and arithmetic on known registers write (computed by the reference emulator's
`rv::compute`); a load or `ecall` (`a0`) makes its register unknown again, and every block starts
with nothing known but `x0`. An arithmetic instruction on known registers becomes `mov rd, k`; a
load or store whose address is known and lies inside guest RAM cannot fault, so it needs no
`mov pc`: an aligned word is a single HLA `load rd, [k]` or `store [k], rs2` (about 200 steps
instead of 4 000), anything else enters the subroutine after its range check. An instruction
that only computes `rd` (`lui`, `auipc`, OP-IMM, OP) is left out when a later one in its block
overwrites `rd` before anything reads it, and before anything that can stop or leave the block, so
the registers are the same wherever the program stops: `lui a0, %hi(x); addi a0, a0, %lo(x)`
is one `mov`.

| instruction | HLA |
|---|---|
| `lui`, `auipc` | `mov rd, k` (`auipc`'s value is computed at build time) |
| OP-IMM, OP | one arithmetic statement; `mov` where the operands are known or the operation an identity (`mv`, `li`) |
| loads, stores | `ea := rs1 + imm`, `mov pc`, the runtime's subroutine (section 8); at a known address inside guest RAM, `load`/`store` or the subroutine without `mov pc` |
| branches | `xor`, `slt` or `sltu`, then `bz`/`bnz` to the target's block; one `bz`/`bnz` against `x0` |
| `jal` | `mov rd, pc + 4`, `jmp` to the target's block |
| `jalr` | as `jal` for a constant target, otherwise through the dispatch table |
| `ecall` | `mov pc`, the runtime's `ecall` |
| `fence` | nothing |
| `ebreak`, illegal | `mov pc` (and `insn`), the stop |

A jump whose target is a constant goes straight to the target's block, or to a stub that stops as
the reference emulator does: misaligned (`stop` 3, `pc` the jump, `tg` the target), outside guest
RAM (6, `pc` the target) or no translated code (8). Running off the end of the code is such a jump.

### The dispatch table

The dispatch table `disp` is an HLA RAM region after guest RAM (and after the padding `dpad` that
aligns it), one word for every guest word of the smallest aligned power-of-two block `[B, B + S)`
that holds all the code: the code address of the block's label for an indirect target, the stop
`f_disp` otherwise. A `jalr` whose target is not a constant computes `t = rs1 + imm` (`add`, unless
`imm` is 0), then

```
and t', t, ~(S - 1) | 2     ; outside the block, or bit 1 set
xor t', t', B
bnz t', stub                ; misaligned, outside guest RAM, or no code: as above
and ix, t, S - 4            ; bit 0 is dropped, as jalr does
or  ix, ix, T               ; T, the table's address, is a multiple of S
mov rd, pc + 4
load t', [ix]
jmp t'
```

about 2 200 steps. `f_disp` recovers the target from `ix` and stops with 8. Guest RAM and the table
together must fit HLA's 2^20 RAM words, which leaves `--ram 4M` no room.

### Differences from the reference emulator

- **Self-modifying code** is not supported. A `fence.i` anywhere in the code, or a store to a
  constant address in the code (its base built by `lui`, `auipc` and `addi` in the block), is a
  build error: `malm build: prog.elf: fence.i at 0x1040: self-modifying code is not supported in aot
  mode`. Other stores into the code go undetected: guest RAM changes, so loads see the new bytes,
  but the translation keeps running the old instructions.
- **Code aot did not translate:** a jump to an aligned address inside guest RAM that is not
  translated code (outside the executable segments, or off their end), or an indirect jump to an
  instruction that is not an indirect target, stops with `stop` 8 (`Untranslated`) and `pc` = the
  target, after the jump wrote `rd`. The reference emulator would run that code. The programs of
  the tests (section 7) never get there.
- The faults are otherwise rv-emu's (section 8): `stop` and `pc` say why and where, the exit status
  is 1.

### Costs

VM steps per instruction from `rv_aot_test` (each a run of 2 to 8 copies in its own block of a
small program, the copies writing different registers; release build), next to rv-emu's
(section 8):

| instruction | aot | emu |
|---|---|---|
| `lui` / `addi rd, rs, 0` (`mv`) | 90 / 140 | 2 200 / 5 500 |
| `addi` / `slti` | 1 900 / 1 860 | 5 500 to 7 300 |
| `ori` / `srli 7` / `slli 7` | 240 / 530 / 910 | 5 500 to 7 300 |
| `add` / `sltu` (register) | 2 010 / 1 940 | 8 500 |
| `xor` / `sll` (register) | 200 / 1 060 | 6 800 / 7 500 to 8 300 |
| `mul` (`y` = `0xF00D`) / `divu` | 33 900 / 117 000 | 50 000 / 125 000 to 130 000 |
| `lui` + `addi` (folded into one `mov`, per pair) | 190 | |
| `lw` / `lb`, `lbu` | 3 970 / 5 100, 4 800 | 9 400 / 10 000 to 10 700 |
| `sw` / `sb` | 3 960 / 6 040 | 10 800 / 12 400 to 12 700 |
| `lui` + `lw` / `lbu` / `sw` at a known address (per pair) | 340 / 1 850 / 380 | |
| `beqz` / `beq` / `bltu` not taken | 240 / 480 / 2 890 | 5 300 to 7 800 |
| `bne` taken | 480 | 11 400 to 14 000 |
| `jal` | 3 | 11 600 |
| `jalr` (through the table) | 1 900 (3 780 for a call and its return) | 10 800 |
| `ecall` (`brk`, with two `li`) | 4 390 | 17 000 |

The arithmetic statements are HLA's (hla.md 4.7), slightly dearer with the many call sites of a
real program; a load or store adds the address `add` (1 750) and the range check (`and`, then
`bnz` on zero, which tests all four bytes: 750). A comparison branch that falls through on a zero
word pays that `bnz` too.

Programs, 64 KiB of guest RAM unless noted (release build; times measured back to back on a
desktop machine; [aot-benchmarks.md](aot-benchmarks.md) tracks the sizes and steps of aot mode):

| program | mode | `.malm` | Malbolge text (and startup steps) | build | guest instructions | VM steps |
|---|---|---|---|---|---|---|
| `hello.c` | emu / aot | 0.68 / 0.67 MB | 30 M / 25 M | 3.2 / 3.5 s | 899 | 38 M / 28 M |
| `cat.c` (9 bytes) | emu / aot | 0.69 / 0.72 MB | 30 M / 29 M | 3.1 / 3.8 s | 1 628 | 44 M / 33 M |
| `fizzbuzz.c` (15) | emu / aot | 0.78 / 2.21 MB | 31 M / 65 M | 4.0 / 8.3 s | 6 740 | 92 M / 85 M |
| `fizzbuzz.c` (2000) | emu / aot | | | | 848 167 | 8.00 G / 2.86 G |
| `runtime_test.c`, 256 KiB | aot | 5.5 MB | 167 M | 21 s | 3 120 348 | 6.21 G |

Without startup, fizzbuzz runs about 9 400 steps per guest instruction in emu mode and 3 300 in aot
mode, 2.8 times faster; the runtime's self-check takes about 2 000 per instruction in aot mode,
startup included, against rv-emu's 8 740 (section 8, at 1 MiB). The price is size: emu mode's
program depends on the guest RAM only, while aot mode adds about 24 K characters of Malbolge text,
and as many startup steps, per guest instruction (0.94 KB of `.malm`; 54 K and 2.2 KB before the
outlining and constants of issue #31), so a program of about 300 instructions has a `.malm` as
large in both modes and fizzbuzz's 1 896 make 65 M characters.
