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
| `asm` | `--check` and `--print` only | parse, check and pretty-print a `.mua` file; assembling is not implemented yet |
| `hla`, `disasm`, `build` | not implemented yet | exit 2 with `malm <cmd>: not implemented` |

The program file of `run`, `export` and `pack` is a `.malm` container ([malm-format.md](malm-format.md))
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
usage: malm asm (--check | --print) [--] <file.mua>
       malm asm [--text] [--map <file>] -o <out> [--] <file.mua>   (not implemented yet)
```

The front end of the assembler ([asm.md](asm.md), issue #13): lexer, parser, `.INCLUDE` expansion
and the static checks of asm.md section 9. Implementation: `src/asm/` (`lexer`, `parser`, `check`,
`printer`) and `src/driver/asm_cmd.cpp`; tests: `tests/asm/`.

| option | meaning |
|---|---|
| `--check` | parse the file (and everything it includes) and run the static checks; prints nothing on success |
| `--print` | after the same checks, print the program in canonical form on stdout |

Diagnostics go to stderr as `file:line:col: error: message`, followed by `file:line:col: note: ...`
lines (the first definition of a duplicate label, and one `included from here` note per include
level), in source order. Independent syntax errors are all reported; the static checks (unknown
and duplicate labels, `R_`/`U_` rules, `ENTRY`, cycles that exist nowhere, ...) run only when the
parse was clean, so that one syntax error does not cascade into a wall of unknown labels. Exit
codes: 0 for a clean file, 1 for diagnostics or a file that cannot be read, 2 for a usage error.

`--print` flattens `.INCLUDE`s (the output holds the blocks of the included files inline), drops
comments and keeps the source's grouping of cells into lines; it is a fixed point: printing the printed output gives the
same text. A plain `malm asm <file> -o <out>` exits 2 with `assembly not implemented yet
(#14-#16); use --check or --print`; layout, startup code and the `.malm` output come with those
tickets.
