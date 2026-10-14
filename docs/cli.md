# The `malm` command line

One binary, `malm`, with subcommands. Implementation: `src/driver/`; tests: `tests/driver/`.

```
usage: malm <command> [args...]
       malm --help | --version
```

| command | status | purpose |
|---|---|---|
| `run` | implemented | run a `.malm` or Malbolge text program |
| `export` | implemented | convert between `.malm` and plain Malbolge text |
| `pack` | implemented | pack a program into a native executable |
| `asm` | implemented | assemble a `.mua` file into a `.malm` or Malbolge text; check and pretty-print it |
| `disasm` | implemented | turn a program back into readable assembly, or list it cell by cell |
| `hla` | implemented | compile a high-level `.mhl` file into a `.malm` or Malbolge text; check it and print the generated assembly |
| `build` | implemented for C and RV32IM ELF input (emu and aot mode) | build a C program or a RISC-V ELF into a `.malm` or a native executable; other languages follow |
| `rvref` | implemented, hidden | run an RV32IM ELF on the C++ reference emulator (a debugging tool; not listed by `--help`) |
| `rvemu` | implemented, hidden | write the HLA program of rv-emu, the RV32IM emulator, optionally with a guest program in its RAM (a build and debugging tool; not listed by `--help`) |
| `rvaot` | implemented, hidden | write the HLA program that rv-aot translates an RV32IM ELF into (a debugging tool; not listed by `--help`) |

The program file of `run`, `export`, `pack` and `disasm` is a `.malm` container ([malm-format.md](malm-format.md))
or Malbolge text (spec 4); the two are told apart by the first four bytes. For a `.malm` the header
decides the mode, and `--classic` must agree with it. For text, `--classic` selects classic mode
(spec 8). `--max-cells N` is a resource cap on the cells a reader accepts (default 2^30, a positive
integer); it is combined with the format limit of the mode (3^38 unleashed, 59049 classic). A lone
`--` ends option parsing, so a file name may start with `-`.

## Exit codes

| code | meaning |
|---|---|
| 0 | success; a halting program, unless `--exit-a` |
| `A mod 256` | `run --exit-a` (or the `EXIT_A` header flag) after a halt |
| 1 | a runtime fault (`E_EXEC`, `E_ENCRYPT`, `E_WIDTH`, `E_IO`), `E_LOAD`, or a file error; a diagnostic goes to stderr |
| 2 | usage error: unknown command or option, missing argument |

## `malm run`

```
usage: malm run [--classic] [--exit-a] [--max-cells N] [--stats]
                [--trace [--trace-from N] [--trace-to M]] [--] <file>
```

Runs the program on stdin and stdout (binary-safe). A fault prints
`malm: E_xxx at step S: C=... D=... A=... n=...` (spec 7) on stderr and exits 1.

| option | meaning |
|---|---|
| `--classic` | classic Malbolge (spec 8) |
| `--exit-a` | a halt exits with `A mod 256` (OR-ed with the `.malm` flag) |
| `--max-cells N` | reader cap on the program's cells |
| `--trace` | one line per step on stderr (below) |
| `--trace-from N`, `--trace-to M` | inclusive 1-based window of steps to trace; each needs `--trace` |
| `--stats` | a statistics block on stderr after the run (below) |

`--trace-from` and `--trace-to` take positive decimal integers (steps are numbered from 1); the
defaults are the whole run. `--trace-from` greater than `--trace-to`, a malformed number, or either
option without `--trace` is a usage error (exit 2). The options can be combined freely and appear
in any order.

Stdout carries only the program's output, byte for byte as without these options, and the exit
status is the same. Everything described here goes to stderr.

A packed executable (`malm pack`) ignores its command line, so it has neither `--trace` nor
`--stats`.

### Trace format

One line per executed step, written before the step executes:

```
step=<S> C=<C> [C]=<cell> op=<op> D=<D> [D]=<cell at D> A=<A> n=<n>
```

* `S`: the step number from 1, the same counter as `at step S` in diagnostics. The step of a fault
  is traced too.
* `C`, `D`, `A`, `[D]`: the register and cell values at the start of the step, in the notation of
  spec 1 (`0t...` for non-negative integers, `...h...` for values with a repeating head 1 or 2).
  Classic words are natural numbers, so they print as `0t...` (`0t1111111111`, never a head
  form), like in diagnostics.
* `[C]`: the code of the cell at C as a plain decimal number, 33..126 (the character is not
  printed).
* `op`: the decoded instruction: `Jmp`, `Out`, `In`, `Rot`, `MovD`, `Opr`, `Nop` or `Hlt`.
* `n`: the memory-size rule at the start of the step (always 10 in classic mode).

`E_EXEC` is detected at the fetch check, so the instruction is not decoded: the line has `op=?`
and `[C]` shows the offending value in the value notation instead of a decimal code, e.g.
`step=4 C=0t10 [C]=...10022 op=? D=0t10 [D]=...10022 A=...2 n=10`.

Lines are buffered and written out before anything else is printed on stderr, so the last trace
line always comes before a fault diagnostic and before the statistics. The buffer is also flushed
before every `In` step, so the trace is current while the program waits for input. Lines still in
the buffer are lost if the process is killed (SIGINT, `timeout`).

Stdout and the trace are separate streams: the trace is not interleaved with the program output in
step order (when both go to a terminal, the order depends on buffering). A write error on stderr
(closed, `/dev/full`, a broken pipe) is ignored: tracing stops, and stdout and the exit status are
unaffected.

### Statistics block

Printed after the run ends, whether by halt, fault or `E_IO`, after the fault diagnostic if there is
one. It is not printed when the program does not load (`E_LOAD`, a file error); there is no run
to report then.

```
malm stats:
  steps        45
  halted       yes
  n            10
  pages        1
  op Jmp       1
  op Out       13
  op In        0
  op Rot       8
  op MovD      1
  op Opr       18
  op Nop       3
  op Hlt       1
  wall         0.000148 s
  steps/s      303398
```

Each row is two spaces, a label padded to 13 columns, then the value. The rows and their order are
fixed. Everything up to `op Hlt` is deterministic; only the last two rows (timing) vary between
runs, so tests compare the block without them.

| row | meaning |
|---|---|
| `steps` | steps executed, including a faulting step |
| `halted` | `yes`, or `no (E_xxx)` naming the fault |
| `n` | the final n; n never shrinks, so this is also the peak (always 10 in classic mode) |
| `pages` | unleashed: the number of materialised memory pages (3^8 cells each; written pages only); classic: `-` |
| `op <name>` | steps per decoded instruction, in the order `Jmp Out In Rot MovD Opr Nop Hlt`; a step that fails with `E_EXEC` is not decoded and so counted in `steps` only; every other step, including one that faults later, is counted. Without `E_EXEC` the counts add up to `steps` |
| `wall` | wall-clock seconds of the run (excluding loading) |
| `steps/s` | `steps` divided by `wall`, rounded down (0 if `wall` is 0); it measures the loop with the observer attached, a plain run is faster |

Example of a fault with both options (`tests/corpus/exec_fault.mal`):

```
$ malm run --trace --stats tests/corpus/exec_fault.mal </dev/null >/dev/null
step=1 C=0t0 [C]=117 op=In D=0t0 [D]=0t11100 A=0t0 n=10
step=2 C=0t1 [C]=98 op=Out D=0t1 [D]=0t10122 A=...2 n=10
step=3 C=0t2 [C]=66 op=Nop D=0t2 [D]=0t2110 A=...2 n=10
step=4 C=0t10 [C]=...10022 op=? D=0t10 [D]=...10022 A=...2 n=10
malm: E_EXEC at step 4: C=0t10 D=0t10 A=...2 n=10
malm stats:
  steps        4
  halted       no (E_EXEC)
  n            10
  pages        1
  op Jmp       0
  op Out       1
  op In        1
  op Rot       0
  op MovD      0
  op Opr       0
  op Nop       1
  op Hlt       0
  wall         0.000023 s
  steps/s      172607
```

The golden traces `tests/programs/hello.classic.trace`, `hello.unleashed.trace` and
`beer99.unleashed.trace` (4258 lines, through the E_WIDTH step) pin the format;
they were produced from the executable model of the spec, not from the VM.

## `malm export`

```
usage: malm export (--text|--malm) [--classic] [--exit-a] [--codec packed|zstd]
                   [--max-cells N] -o <out> [--] <in>
       (-o - writes to stdout; --exit-a and --codec apply to --malm only)
```

`--malm` writes a `.malm` (default codec `zstd`; `--exit-a` sets the header flag), `--text` writes
the program as Malbolge text with one trailing newline. Converting a `.malm` that is classic or has
`EXIT_A` to text prints a warning, because text cannot carry either. Exit codes as above.

## `malm pack`

```
usage: malm pack [--classic] [--exit-a] [--max-cells N] -o <out> [--] <in>
```

Writes a native executable: a copy of the running `malm` with the program appended as a `.malm`
payload. The result runs the embedded program on stdin and stdout, ignores its command line and
exits as `malm run` does. `-o -` is not supported. `pack` refuses to overwrite the running
binary or the input.

## `malm asm`

```
usage: malm asm [--text] [--map <file>] -o <out> [--] <file.mua>
       malm asm (--check | --print) [--] <file.mua>
```

The assembler ([asm.md](asm.md)): lexer, parser, `.INCLUDE` expansion, the static checks of
asm.md section 9, layout and the startup code (asm.md section 7). Implementation: `src/asm/`
and `src/driver/asm_cmd.cpp`; tests: `tests/asm/`. Golden examples: `examples/asm/`.

| option | meaning |
|---|---|
| `-o <out>` | write the assembled program, a `.malm` (zstd); `-o -` writes it to stdout |
| `--text` | write Malbolge text instead (one trailing newline), as `malm export --text` would |
| `--map <file>` | also write the label map (below); not to stdout |
| `--check` | parse the file (and everything it includes) and run the static checks; prints nothing on success |
| `--print` | after the same checks, print the program in canonical form on stdout |

The output is pure (spec 4): before writing, `malm asm` checks that the program text passes the
strict loader unchanged and that the `.malm` holds exactly these cells (unleashed mode, no
`EXIT_A`, no other data); a failure would be an assembler bug and exits 1. Assembling the same
source gives the same bytes. `malm asm` refuses to write over its input file, and `-o` and `--map`
must name different files.

The label map has one line per label, ordered by address and then by name:
`name address code|data first last`, where `address` is the label's cell and `first`/`last` the
extent of its block, all in the notation of spec 1 (`0t...`, `...1...`).

Diagnostics go to stderr as `file:line:col: error: message`, followed by `file:line:col: note: ...`
lines (the first definition of a duplicate label, and one `included from here` note per include
level), in source order. Independent syntax errors are all reported; the static checks (unknown
and duplicate labels, `R_`/`U_` rules, `ENTRY`, cycles that exist nowhere, ...) run only when the
parse was clean, so that one syntax error does not cascade into a wall of unknown labels. Layout
errors (asm.md section 9) are reported when assembling. Exit codes: 0 on success, 1 for
diagnostics, a file that cannot be read or written, 2 for a usage error. Nothing is written when
there are diagnostics.

`--print` flattens `.INCLUDE`s (the output holds the blocks of the included files inline), drops
comments and keeps the source's grouping of cells into lines; it is a fixed point: printing the
printed output gives the same text.

## `malm hla`

```
usage: malm hla [--text] [--map <file>] -o <out> [--] <file.mhl>
       malm hla (--check | --print) [--] <file.mhl>
```

The high-level assembler ([hla.md](hla.md)): parses the file, lowers it to an assembly program
and assembles that like `malm asm`. Implementation: `src/hla/` and `src/driver/hla_cmd.cpp`;
tests: `tests/hla/`. Examples: `examples/hla/`.

| option | meaning |
|---|---|
| `-o <out>` | write the compiled program, a `.malm` (zstd) with the `EXIT_A` flag, so `halt <code>` sets the exit status; `-o -` writes it to stdout |
| `--text` | write Malbolge text instead; text cannot carry `EXIT_A`, so a warning on stderr says to run it with `malm run --exit-a` |
| `--map <file>` | also write the label map of the generated assembly (as `malm asm --map`) |
| `--check` | parse, check and lower the file and run the assembler's static checks on the result; prints nothing on success |
| `--print` | after the same checks, print the generated assembly in canonical form on stdout; `malm asm` assembles it to the same program text |

Diagnostics, exit codes, purity check and determinism are those of `malm asm`; diagnostics point at
the `.mhl` file, also for errors the assembler finds in the generated program.

## `malm build`

```
usage: malm build [--mode emu|aot] [--ram <size>] [--cc <compiler>]
                  (-o <out.malm> | --exe -o <out>) (<prog.c>... | <prog.elf>)
                  [-- <compiler args>...]
```

Builds a C program, or an RV32IM ELF executable, into a pure `.malm` that runs it in emu mode
(rv-emu with the program in its guest RAM, [rv.md](rv.md) section 8) or in aot mode (the program
translated instruction by instruction, rv.md section 9). The guest's stdin, stdout and exit status
are the program's, through `malm run` or the native executable of `--exe`.
Implementation: `src/driver/build_cmd.cpp` and `src/driver/toolchain.cpp`; tests:
`tests/rv/build_test.cpp`, `tests/rv/toolchain_test.cpp`, `tests/rv/build_pipe_test.sh` and
`tests/rv/rv_pipe_test.sh`.

| option | meaning |
|---|---|
| `--mode emu` | run the program in rv-emu, the RV32IM emulator (the default) |
| `--mode aot` | translate the program ahead of time: about three times faster (rv.md section 9), and larger by about 24 K characters of Malbolge text per guest instruction; no self-modifying code |
| `--ram <size>` | guest RAM in bytes, or with a `K` (KiB) or `M` (MiB) suffix; a multiple of 16 up to `4M` (HLA's RAM limit); default `1M` |
| `--cc <compiler>` | the C compiler: a path, or a name looked up in `PATH`; default `clang`, else the newest `clang-N` in `PATH` |
| `-o <out.malm>` | where to write the program, a `.malm` (zstd) with the `EXIT_A` flag; `-o -` writes it to stdout |
| `--exe -o <out>` | write a native executable instead, as `malm pack` would (not to stdout) |
| `-- <args>...` | everything after `--` goes to the compiler; an argument `-Wl,a,b` passes `a` and `b` to the linker instead |

The input is one or more C sources (files ending in `.c`), compiled and linked together, or one
ELF executable (recognized by its first four bytes), loaded like `malm rvref` loads it (rv.md
section 1). Because `--` starts the compiler arguments, a source whose name starts with `-` is
written `./-name.c`.

C sources are compiled with clang for `riscv32-unknown-elf`, `rv32im`, `ilp32`, `-O2` (the
arguments after `--` come later, so `-O0` overrides it), against the runtime's headers, and linked
by `ld.lld` with crt0, the linker script and the libc of [rv.md](rv.md) section 6, as described
there. The runtime is built into `malm`, so nothing besides clang and ld.lld is needed: the
runtime is compiled anew in a temporary directory for every build (about a quarter of a second).
`ld.lld` is looked for next to the compiler, after resolving symlinks (which finds Ubuntu's
`/usr/lib/llvm-N/bin/ld.lld` for `clang-N`), then as `ld.lld` or the newest `ld.lld-N` in `PATH`.
The compiler's and the linker's diagnostics and warnings for the program go to stderr.

Exit codes: 0; 1 if clang or ld.lld is missing (`malm build: no C compiler: ...`,
`malm build: no linker: ...`), the compiler cannot target riscv32 (`malm build: <cc> cannot
compile for riscv32-unknown-elf ...`, after the compiler's own message), compiling or linking
fails, the ELF loader rejects the program (`malm build: <file>: <reason>`), aot mode cannot
translate it (`malm build: <file>: fence.i at 0x...: self-modifying code is not supported in aot
mode`, or guest RAM and the dispatch table exceed HLA's RAM), a file cannot be read or written, or
the output would overwrite an input; 2 for a usage error.
The output passes the purity check of `malm asm`, and the same ELF and options give the same bytes.

Building compiles the whole emulator: about 25 seconds with the default 1 MiB of guest RAM and 4 with
64 KiB (release build); aot mode adds about 2.5 seconds per thousand guest instructions. Running needs about 5 GB of host memory with 1 MiB (rv.md section 8), so
small programs are better built with a smaller `--ram`. The heap ends `__stack_size` (64 KiB) below
the stack (rv.md section 6): with 64 KiB of guest RAM a program that calls `malloc` needs
`-- -Wl,--defsym=__stack_size=16384` or similar.

```
$ malm build --ram 64K --exe -o cat examples/c/cat.c
$ echo hi | ./cat
hi
$ malm build --ram 64K -o hello.malm hello.elf
$ malm run hello.malm
Hello, world!
$ malm build --mode aot --ram 64K --exe -o fizzbuzz examples/c/fizzbuzz.c
$ echo 3 | ./fizzbuzz
1
2
Fizz
```

## `malm disasm`

```
usage: malm disasm [--map <file>] [--raw] [--classic] [--max-cells N] [-o <out>] [--] <file>
```

The disassembler, for debugging programs from `malm asm`. Implementation: `src/asm/disasm.cpp` and
`src/driver/disasm_cmd.cpp`; tests: `tests/asm/disasm_test.cpp`.

| option | meaning |
|---|---|
| `--map <file>` | take labels and blocks from the label map of `malm asm --map` |
| `--raw` | list the program text cell by cell, even for a program from `malm asm` |
| `-o <out>` | write the output to `<out>`; the default (and `-o -`) is stdout |

The text of an assembled program is only its startup code (asm.md section 7), so `malm disasm`
runs it, without I/O, up to its final `Jmp` into `ENTRY`, and reads the memory image the startup
code wrote. From that it writes a `.mua` that `malm asm` turns back into the same program text:

- `.WIDTH` is the program width, and every block is pinned with `@` at its address;
- code cells are written as the cycle their character performs at its position (`Rot/Nop`,
  `Nop/MovD`, `RNop`, ...), or as a single command for a 68-cycle; each one is annotated with its
  address, position mod 94 (`p=`), character and xlat2 cycle (`2-cycle #1`: the cycle's length
  and the character's index in it);
- data words that point into a block are written as label references, `X` or `R_X`, the rest as
  constants (`'H'<<1` where that is what rotating the value gives); data lines follow the units
  they invoke, as in `ROT 'H'<<1 R_ROT`.

With `--map` the labels and blocks are the map's. Without it, code is what the data-driven
execution model reaches from `ENTRY` without running the program (both states of every flag
included), code blocks are named after their first unit (`ROT`, `MOVED`, `FLAG`, `SLED`, ...),
and other labels after their address (`L0t101112110`); cells that are only reached through
values computed at run time, such as branch sleds, come out as data. Either way, `malm disasm`
checks in-process that its output lays out to the same image and prints
`malm disasm: warning: ...` on stderr if it does not.

Any other program (classic mode, `--raw`, or a text whose run does I/O, halts or faults before a
second `Jmp`, or does not reach one within as many steps as it has cells) is listed instead:
one line per cell with its address, position, character, normalized instruction letter
(research 4.1), decoded instruction and xlat2 cycle.

Exit codes: 0 on success (also with a warning), 1 for a file that cannot be read or written and
for a label map that is malformed or does not belong to the program, 2 for a usage error.
`malm disasm` refuses to write over its input file or the label map.

## `malm rvref`

```
usage: malm rvref [--ram <size>] [--trace] [--regs] [--] <prog.elf>
```

Runs an RV32IM ELF on the C++ reference emulator, the oracle for emu and aot mode, with the
guest's stdin and stdout on the process's (binary-safe). The guest contract (loader rules, RAM,
system calls, faults) is [rv.md](rv.md). Implementation: `src/rv/` and
`src/driver/rvref_cmd.cpp`; tests: `tests/rv/`. A debugging tool, so `malm --help` does not list
it.

| option | meaning |
|---|---|
| `--ram <size>` | guest RAM in bytes, or with a `K` (KiB) or `M` (MiB) suffix; a multiple of 16 up to `1024M`; default `1M` |
| `--trace` | one line per instruction on stderr (below) |
| `--regs` | after the run, the final `pc`, the step count and the 32 registers on stderr |

Exit codes: the guest's `exit` status (`code & 255`); 1 for a fault, an ELF the loader rejects
or a file that cannot be read, each with a diagnostic on stderr; 2 for a usage error. A fault
prints `malm rvref: <reason> at pc=0x... (step S)` (rv.md section 5).

The trace has one line per executed instruction, the faulting one and the final `exit` ecall
included: `step=S pc=0x... insn=0x...`, followed by ` <reg>=0x...` (ABI name) for each register
the instruction changed. Lines are buffered and written before every `read` and at the end, so
they come before a fault diagnostic and before the `--regs` block:

```
$ malm rvref --trace --regs hello.elf 2>&1 >/dev/null | head -3
step=1 pc=0x00001000 insn=0x000015b7 a1=0x00001000
step=2 pc=0x00001004 insn=0x02c58593 a1=0x0000102c
step=3 pc=0x00001008 insn=0x00e00613 a2=0x0000000e
```

The `--regs` block is `pc   0x...`, `steps N` (instructions completed; the `exit` ecall counts, a
faulting instruction does not), then one line per register, `x<i> <abi name>` padded to 9
columns followed by the value: `x2 sp    0x00100000`.

## `malm rvemu`

```
usage: malm rvemu [--ram <size>] [--harness] [-o <out.mhl>] [--] [<prog.elf>]
```

Writes the HLA program of rv-emu, the RV32IM emulator written in HLA ([rv.md](rv.md) section 8),
for `malm hla` to compile. The build makes `emu.malm` with it (`rv/emu/CMakeLists.txt`).
Implementation: `src/rv/emu.cpp`, `src/rv/semantics.cpp` and `src/driver/rvemu_cmd.cpp`; tests:
`tests/rv/`. A build and debugging tool, so `malm --help` does not list it.

| option | meaning |
|---|---|
| `--ram <size>` | guest RAM in bytes, or with a `K` (KiB) or `M` (MiB) suffix; a multiple of 16 up to `4M` (HLA's RAM limit); default `1M` |
| `--harness` | a stop (fault or `exit`) sets the register `stop` and continues at the label `fetch` instead of halting, so a test can run case after case in one VM |
| `-o <out.mhl>` | where to write the program (`-` or no `-o`: stdout) |
| `<prog.elf>` | a guest program (loaded like `malm rvref` loads it): its segments are the initial RAM, `pc` starts at its entry and the program break at its end; without one, RAM is zero and `pc` starts at 0 |

Exit codes: 0; 1 if the ELF loader rejects the file or a file cannot be read or written, with a
diagnostic on stderr; 2 usage error.

`malm build` does both steps in one; with `malm rvemu`, the generated program can be read or
compiled with `malm hla --map`:

```
$ malm rvemu --ram 64K hello.elf -o hello.mhl && malm hla -o hello.malm hello.mhl
$ malm run hello.malm
Hello, world!
```

## `malm rvaot`

```
usage: malm rvaot [--ram <size>] [-o <out.mhl>] [--] <prog.elf>
```

Writes the HLA program that rv-aot translates an RV32IM program into ([rv.md](rv.md) section 9),
for reading or for `malm hla` to compile; `malm build --mode aot` does both steps in one.
Implementation: `src/aot/` and `src/driver/rvaot_cmd.cpp`; tests: `tests/rv/aot_test.cpp`. A
debugging tool, so `malm --help` does not list it.

| option | meaning |
|---|---|
| `--ram <size>` | guest RAM in bytes, or with a `K` (KiB) or `M` (MiB) suffix; a multiple of 16 up to `4M` (HLA's RAM limit, which the dispatch table shares); default `1M` |
| `-o <out.mhl>` | where to write the program (`-` or no `-o`: stdout) |
| `<prog.elf>` | the program, loaded like `malm rvref` loads it |

Exit codes: 0; 1 if the ELF loader rejects the file, aot mode cannot translate it (as
`malm build`), or a file cannot be read or written, with a diagnostic on stderr; 2 usage error.

```
$ malm rvaot --ram 64K hello.elf -o hello.mhl && malm hla -o hello.malm hello.mhl
$ malm run hello.malm
Hello, world!
```
