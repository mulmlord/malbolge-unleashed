# malbolge-unleashed high-level assembly (`.mhl`)

Status: language core, issue #19. Calls, jump tables and RAM (#20) and the arithmetic library (#21,
#22) extend it.
Sources:
- the assembly language it lowers to: `docs/asm.md`
- the word representation: `docs/adr/0001-hla-word-representation.md`
- the implementation: `src/hla/` (`parser`, `word`, `lower`); the command: `malm hla` (docs/cli.md)

HLA is a small register language. A program declares 32-bit registers and runs statements on them:
moves, jumps, branches on zero, and byte I/O. `malm hla` lowers it to an assembly program
(`.mua`, an AST of `src/asm`), which the assembler lays out and turns into a pure `.malm` with the
`EXIT_A` flag set.

```
; Cat: copies stdin to stdout and halts at end of input.
        reg c
loop:   in c, done          ; c := the next byte; at EOF go to done
        out c
        jmp loop
done:   halt
```

## 1. Machine model

- **Registers** hold 32-bit words. Each register is one memory cell (ADR 0001): bit `i` is trit
  `4 + i`, stored as 0 or 2. Registers are declared with `reg` and start at their declared value
  (default 0); the startup code writes them (the program is pure, spec 4).
- **Control** runs statement by statement. Labels name statements; `jmp`, `bz`, `bnz` and the EOF
  branch of `in` go to them.
- **I/O** is byte-wise and binary: `in` reads one byte (EOF is a value, see 3), `out` writes the low
  byte of a word.
- **Halting** sets `A` to the exit code and executes `Hlt`. `malm hla` always sets the container
  flag `EXIT_A`, so the process exits with that code (spec 7, docs/malm-format.md). Running off the
  end of the program is `halt` (code 0).
- **Width.** HLA programs run at `n = 38` (`.WIDTH 38`): a word cell has width 36, and the
  dispatch tables (4.4) use addresses up to width 37.

## 2. Lexical structure and syntax

- ASCII, case-sensitive. Comments run from `;`, `#` or `//` to the end of the line.
- **Lines.** A line holds any number of labels (`name:`), then at most one statement or one `reg`
  declaration. Labels may stand on lines of their own; they name the next statement (or the end of
  the program).
- **Identifiers:** `[A-Za-z_][A-Za-z0-9_]*`, at most 64 characters. Registers and labels share one
  namespace; every name is defined once. The mnemonics and `reg` are reserved.
- **Numbers** are 32-bit words: decimal (`42`, `-1` = `0xFFFFFFFF`), hexadecimal (`0x2A`) or binary
  (`0b101010`), from `-2^31` to `2^32 - 1`. A character literal `'c'` is the byte's value. Escapes,
  in characters and strings: `\n \r \t \0 \\ \' \"` and `\xHH`; other control or non-ASCII bytes
  need an escape.
- **Strings** `"..."` (at least one byte) are only an operand of `out`.

```
line      := { ident ":" } [ statement | declaration ]
declaration := "reg" ident [ "=" number ] { "," ident [ "=" number ] }
statement := mnemonic [ operand { "," operand } ]
operand   := ident | number | string
```

## 3. Statements

`r`, `s` are registers, `L` a label, `k` a number, `b` a byte (a number 0..255).

| Statement | Effect |
|---|---|
| `reg r [= k], ...` | declares registers (anywhere in the file; not executed) |
| `mov r, k` | `r := k` |
| `mov r, s` | `r := s` (`mov r, r` does nothing) |
| `jmp L` | continue at `L` |
| `bz r, L` | if `r = 0`, continue at `L` |
| `bnz r, L` | if `r != 0`, continue at `L` |
| `in r` | `r :=` the next input byte (0..255), or `0xFFFFFFFF` at end of input |
| `in r, L` | the same, and at end of input continue at `L` |
| `out r` | write the low byte of `r` |
| `out b`, `out "text"` | write the byte, or the bytes of the string |
| `halt` | stop with exit code 0 |
| `halt b` | stop with exit code `b` |
| `halt r` | stop with the low byte of `r` as exit code |

The exit code needs `EXIT_A`: a `.malm` from `malm hla` has it; Malbolge text (`--text`) cannot carry
it, so run text with `malm run --exit-a`.

## 4. Lowering

The lowering is the contract between HLA and the assembler; the generated program can be read with
`malm hla --print`. It uses the HeLL idioms of docs/asm.md 3.3 and 6.

### 4.1 Names and blocks

- User names get a prefix: label `x` becomes `L_x`, register `x` the data label `V_x` (its value
  cell) with the entry `V_x__O`. Generated names start with `_`.
- **Code units** (only those the program uses): `_MOVED`, `_ROT`, `_OPR`, `_INP`, `_OUTP`, `_MD`,
  `_GO` (each `X/Nop` + `Jmp`), `_HALT` (`Hlt`), and the flags `_AFk`, `_SFk` (`Nop/MovD` + `Jmp`).
- **The main stream** is the data block `ENTRY`, one line per primitive below. Every HLA label is a
  landing pad `L_x: R__MOVED`; a statement that can fall through into a label first jumps to it
  (`_MOVED L_x`), so the pad always finds `_MOVED` used.
- Every generated cell carries the source location of its statement, every support cell (register
  blocks, routines, tables) the location of its declaration or first use. Diagnostics of the
  assembler therefore point into the `.mhl` file.

### 4.2 Primitives

- **Load** `A := v` (every value of width <= 38 with head 0 or 1): `_ROT C0 R__ROT` or
  `_ROT C1 R__ROT` (rotation fixed points), then up to four `_OPR k R__OPR` on inline constants
  `k` that are fixed points of the `A` they meet (docs/asm.md 10.3). No cell changes.
- **Access** `A := r := crz(A, r)`: a call `_MOVED V_r__O` into the register's block
  `V_r__O: _OPR V_r: <value> R__OPR R__MOVED <return tree>`, which returns to a pad `R__MOVED` after
  the call. With `A = C1` an access reads `r` into `A` and leaves it unchanged.
- **Reset** `r := C1` from any value: load `C1`, access `r` twice.
- `mov r, k`: reset `r`, load the `a` with `crz(a, C1) = word(k)` (all trits 1, the set bits 2), access
  `r`. `mov r, s`: reset `r`; access `s` (`A := s`); access `r` (`r := crz(s, C1)`: bits 0 become 1);
  load `C1`; access `r` (the 1s become 0).
- `out b`: load `b`, `_OUTP ?- R__OUTP`. `halt [b]`: load `b` (or 0), `_HALT`.

### 4.3 Return trees

A register, routine or table is entered from many call sites and must return to the right one. It
returns through a binary tree of flags: the node at depth `d` tests flag `F_d` (`F_d upper R_F_d`,
docs/asm.md 3.3); a set flag sends execution to the upper half and clears itself, a clear one falls
through to the lower half. Leaves are `_MOVED <return pad>`. The caller sets, just before the call,
the flags of the depths where its leaf lies in the upper half. A return costs about 2.5 steps per
level, `log2(call sites)` levels, instead of the linear chain of one flag per site (ADR 0001,
"Return overhead").

Register accesses use the flags `_AFk`; routines and tables (which perform register accesses
themselves) use `_SFk`, so the two never interfere. Every flag that is tested is clear afterwards.

### 4.4 Dispatch tables

A byte cannot be tested trit-wise, so `out r`, `halt r`, `bz`, `bnz` and `in` dispatch through
tables that the startup code writes (src/hla/word.hpp has the constants; `_MD` is `MovD` through a
register, `_GO` a goto out of a table).

- **Lane index.** For byte lane `l` (bits `8l .. 8l + 7`) of `r`, with a scratch register `_I`:
  `A := C1; access _I; access _I; access r; access _I; A := K; access _I; access _I` leaves `_I` =
  the lane's bits as trits 0/1, the other bits 1, a 4-trit tag in the trits 0..3, the trits 36, 37
  and the head 1, and `r` unchanged. `_MOVED _I__M` then moves `D` through `_I` (`U__MD _I`), so
  the next word is read from the table entry at `_I + 1`. Indices are 81 cells apart.
- **Byte table** (tag 0, lane 0): 256 entries `R__MD R__MOVED <load b> _MOVED _BYTE_RET`;
  `_BYTE_RET` returns with `A = b`. Used by `out r` and `halt r`.
- **Zero tables** (tags 8..14, one per lane): 256 entries `_GO _Z<l>` (byte 0) or `_GO _NZ<l>`.
  `bz`/`bnz` test lane 0, then lanes 1, 2, 3 while they are zero, and stop at the first nonzero
  lane.
- **Input table.** `in` calls the routine `_INSUB`: it sets the scratch register `_S` to a seed
  (`in_seed`), executes `In`, crazies the byte into `_S` and dispatches through `_S`. The 256 bytes
  land on 729 consecutive cells of a head-1 block; each holds `U__GO Y` with `Y` a nearby free cell
  naming the byte's stream, which loads the operand that turns `C1` into the word. EOF lands on a
  block of its own in head 0. The caller has reset `r` and accesses it with that operand.

A program pays for a table only if it uses it: hello world has none, cat the byte and input tables,
a `bz` all four zero tables.

### 4.5 Costs

VM steps per statement, measured with `--trace` in a small program (few call sites; every return
tree level adds about 2.5 steps):

| Statement | Steps |
|---|---|
| `jmp` | 3 |
| `out b` | 5 to 15 |
| `mov r, k` | 51 |
| `mov r, s` | 75 |
| `out r`, `halt r` | 125 |
| `bz`/`bnz`, nonzero in lane 0 | 155 |
| `bz`/`bnz` on 0 (all four lanes) | 605 |
| `in r` | 390 |

Program size (release build): `hello.mhl` 2.5 KB `.malm`; `cat.mhl` 36 KB `.malm` (1.2 M
characters of Malbolge text, 1.2 M startup steps); a program using `bz`, the byte table and the input
table about 70 KB.

## 5. Diagnostics

Errors have the assembler's form, `file:line:col: error: message` plus notes (docs/asm.md 9):
- lexical and syntax errors, one per line (the rest of the line is skipped), all lines reported;
- operands that do not fit the statement, bytes outside 0..255, numbers outside 32 bits;
- unknown names, a register where a label is needed and the reverse, duplicate definitions;
- errors the assembler finds in the generated program, at the HLA statement that produced it.

## 6. Examples

`examples/hla` holds `hello.mhl` (`out "Hello, world!\n"`), `cat.mhl` (above) and `exit42.mhl`
(`halt 42`). `tests/hla/examples_test.cpp` checks that hello and cat produce exactly the output of
`examples/asm/hello.mua` and `examples/asm/halting_cat.mua` (cat also that of the endless
`cat.mua` up to end of input; after it, `cat.mua` writes byte 168 forever while HLA's `in` reports
EOF), and that `exit42.mhl` exits with 42; `tests/hla/examples_pipe_test.sh` does the same through
`malm hla` and `malm run`.
