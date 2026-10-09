# malbolge-unleashed high-level assembly (`.mhl`)

Status: language core (issue #19), calls, the stack, jump tables and RAM (issue #20). The
arithmetic library (#21, #22) extends it.
Sources:
- the assembly language it lowers to: `docs/asm.md`
- the word representation: `docs/adr/0001-hla-word-representation.md`
- the implementation: `src/hla/` (`parser`, `word`, `lower`); the command: `malm hla` (docs/cli.md)

HLA is a small register language. A program declares 32-bit registers and RAM and runs statements
on them: moves, jumps, branches on zero, byte I/O, calls and returns, a stack, jump tables, and
loads and stores through address registers. `malm hla` lowers it to an assembly program
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
- **RAM** is declared in regions of 32-bit words (`ram name[size]`). Addresses are byte addresses:
  the regions follow each other from address 0 in declaration order, and a word is at a multiple
  of 4. `load` and `store` take the address from a register or a constant (ADR 0001: a record per
  word, reached by `crz` from the address, no arithmetic).
- **The stack** holds words: `push`, `pop`, and the return addresses of `call`. Its size is
  declared with `stack n` (words; default 64). Pushing onto a full stack or popping an empty one
  halts with exit code 255. The stack is not RAM: it has no addresses.
- **Code addresses.** A label used as a value (`mov r, L`, `push L`, a RAM word `= L`) is its code
  address, a word that `jmp r` and `call r` continue at. Code addresses can be copied, pushed and
  stored like any word, but they are not numbers: arithmetic on them (#21) is meaningless, and
  jumping through a register that holds anything else is undefined.
- **Halting** sets `A` to the exit code and executes `Hlt`. `malm hla` always sets the container
  flag `EXIT_A`, so the process exits with that code (spec 7, docs/malm-format.md). Running off the
  end of the program is `halt` (code 0).
- **Width.** HLA programs run at `n = 38` (`.WIDTH 38`): a word cell has width 36, and the
  dispatch tables (4.4) use addresses up to width 37.

## 2. Lexical structure and syntax

- ASCII, case-sensitive. Comments run from `;`, `#` or `//` to the end of the line.
- **Lines.** A line holds any number of labels (`name:`), then at most one statement or one
  declaration (`reg`, `ram`, `stack`). Labels may stand on lines of their own; they name the next
  statement (or the end of the program).
- **Identifiers:** `[A-Za-z_][A-Za-z0-9_]*`, at most 64 characters. Registers, labels and RAM
  regions share one namespace; every name is defined once. The mnemonics, `reg`, `ram` and
  `stack` are reserved.
- **Numbers** are 32-bit words: decimal (`42`, `-1` = `0xFFFFFFFF`), hexadecimal (`0x2A`) or binary
  (`0b101010`), from `-2^31` to `2^32 - 1`. A character literal `'c'` is the byte's value. Escapes,
  in characters and strings: `\n \r \t \0 \\ \' \"` and `\xHH`; other control or non-ASCII bytes
  need an escape.
- **Strings** `"..."` (at least one byte) are only an operand of `out`.
- **Values** are numbers, labels (their code address) and RAM regions (their base address).
- **Addresses** are written in brackets: `[r]` (a register), `[k]` (a number) or `[m]` (a RAM
  region: its first word). A constant address must be a word of a declared region.

```
line        := { ident ":" } [ statement | declaration ]
declaration := "reg" ident [ "=" value ] { "," ident [ "=" value ] }
             | "ram" ident "[" number "]" [ "=" value { "," value } ]
             | "stack" number
statement   := mnemonic [ operand { "," operand } ]
operand     := ident | number | string | "[" ( ident | number ) "]"
value       := ident | number
```

## 3. Statements

`r`, `s` are registers, `L` a label, `k` a value (a number, a label or a RAM region), `b` a byte (a
number 0..255), `m` a RAM region, `[a]` an address (`[r]`, `[k]` or `[m]`, section 2).

| Statement | Effect |
|---|---|
| `reg r [= k], ...` | declares registers (anywhere in the file; not executed) |
| `ram m[n] [= k, ...]` | declares a RAM region of `n` words after the previous one; the words start at the values given, the rest at 0 |
| `stack n` | the stack holds `n` words (1..65536; at most one; default 64) |
| `mov r, k` | `r := k` |
| `mov r, s` | `r := s` (`mov r, r` does nothing) |
| `jmp L` | continue at `L` |
| `jmp r` | continue at the code address in `r` |
| `call L`, `call r` | push the return address, then `jmp` |
| `ret` | pop a code address and continue there |
| `push r`, `push k` | push a word |
| `pop r` | `r :=` the popped word |
| `switch r, L0, L1, ...` | continue at `Lb`, `b` the low byte of `r`, if there are more than `b` labels; else with the next statement (1..256 labels) |
| `load r, [a]` | `r :=` the word at address `a` |
| `store [a], s`, `store [a], k` | the word at address `a` := `s` or `k` |
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

`switch` looks at the low byte only, like `out r` and `halt r`: `switch r, a, b` goes to `b` for
`r = 0x101`. An address that is not a multiple of 4 inside a declared region, in a register, is
undefined (the access lands on a cell that is not a word, and the program usually faults);
constant addresses are checked (section 5). Subroutines are plain labels: `call` and `ret` need no
declaration, and recursion works up to the stack size.

## 4. Lowering

The lowering is the contract between HLA and the assembler; the generated program can be read with
`malm hla --print`. It uses the HeLL idioms of docs/asm.md 3.3 and 6.

### 4.1 Names and blocks

- User names get a prefix: label `x` becomes `L_x`, register `x` the data label `V_x` (its value
  cell) with the entry `V_x__O`. Generated names start with `_`.
- **Code units** (only those the program uses): `_MOVED`, `_ROT`, `_OPR`, `_INP`, `_OUTP`, `_MD`,
  `_GO`, `_MB` (each `X/Nop` + `Jmp`), `_HALT` (`Hlt`), the RAM access `_RA` (4.5), and the flags
  `_AFk`, `_SFk` and `_DIR` (`Nop/MovD` + `Jmp`).
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

A byte cannot be tested trit-wise, so `out r`, `halt r`, `bz`, `bnz`, `in` and `switch` dispatch
through tables that the startup code writes (src/hla/word.hpp has the constants; `_MD` is `MovD`
through a register, `_GO` a goto out of a table).

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

- **Switch tables** (one per `switch`). The lane-0 index of `r` (tag 1), crazied with the
  constant `switch_mask(s)` of the switch's number `s`, has the byte's bits as trits 0/2 and `s` in
  the trits of lanes 1..3 (word.hpp), so every switch has 256 entries of its own:
  `R__MD R__MOVED _MOVED <target>`, the target being a label or the statement after the switch.

A program pays for a table only if it uses it: hello world has none, cat the byte and input tables,
a `bz` all four zero tables, every `switch` its own.

### 4.5 RAM, the stack and code addresses

Everything here is pinned at addresses that word.hpp computes; `tests/hla/word_test.cpp` checks
that no two of them, nor the tables of 4.4, share a cell.

- **RAM records** (ADR 0001). The word at byte address `a` is the value cell of the record
  `? <value> _MB _BACK` from `ram_record(a) - 1` (head 1; the bit `i` of `a` is the trit `4 + i`
  as `1 - bit`; the `?` keeps the value out of the first word of a line, docs/asm.md 6). A pointer
  register holds `ram_record(a) - 2`, and its entry `__A` is `U__RA <register>` into
  `_RA: MovD/Nop/Nop/Nop RNop Opr/Nop/Nop/Nop Jmp`: the `MovD` goes through the pointer, the `Opr`
  acts on the record's value, and the `Jmp` reads `_MB`, whose `MovD` reads `_BACK`. `_BACK`
  re-arms `_MB` and `_MOVED`, steps both 4-cycles of `_RA` back with three `R__RA` (each runs the
  RNop and the `Opr` cell as a `Nop`), and returns through a tree (flags `_AFk`, like a register).
- **Pointers.** `load` and `store` build the pointer in the scratch register `_P`: from an address
  register `x` by `reset _P; A := K; access x; access _P; A := R; access x` (`pointer_mask`,
  `pointer_restore`: `x` is unchanged), from a constant by `reset _P; A := operand; access _P`.
- `load r, [a]`: reset `r`, one RAM access (with `A = C1`, it reads the word and keeps it), access
  `r`, load `C1`, access `r` (like `mov`). `store [a], s`: `A := C1`, two RAM accesses (the word
  becomes `C1`), `A := 0`, access `s` (0 -> 1, 2 -> 2), a RAM access (the word becomes `s`),
  `A := C1`, access `s` (restored): three accesses to the word, two to `s`. A constant is loaded
  as the operand that turns `C1` into it, in one RAM access.
- **Stack frames.** Frames `0..n` (`n` = the stack size) are records too, in a family of their own
  (`frame_record`), each `[U__GO Y] [value] _MB _BACK [Y: _fs<k>]` from `frame_record(k) - 1`.
  The stack pointer `_SP` holds `frame_record(k) - 2` with `k` words pushed; RAM accesses go
  through it directly, without a pointer. It moves by a call `_MOVED _SP__M` (`U__MD _SP`): the
  `MovD` lands on the frame's `U__GO Y`, and `Y` names the frame's stream `_fs<k>`, which tests
  the flag `_DIR` (set by the caller for up) and loads the operand that turns the pointer into the
  next or previous frame's; `_SP_RET` applies it to `_SP` and returns (flags `_SFk`). Frame `n`'s
  up and frame 0's down go to `_SOVF`, which halts with 255. `push` = store at the pointer, then
  up; `pop` = down, then load.
- **Code addresses.** `code_address(id)` (id from 1) is an f02 value with head 0 and the trit 36 =
  2, so it is copied like a word and its pad at `+ 1` lies far above the first-fit area. The pad
  is `R__MD R__MOVED _MOVED <label>`, entered by a `MovD` through a dispatch register
  (`_MOVED V_r__M`). Labels used as values and the return points of `call` get one.
- `call L`: push the code address of a fresh return point (a store of a constant), `_MOVED L`;
  the return point is a landing pad `R__MOVED`. `ret`: down, load the word into the scratch
  register `_R`, `_MOVED _R__M`. `jmp r`: `_MOVED V_r__M`.

### 4.6 Costs

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
| `jmp r` | 10 |
| `call L` (to the label) | 170 |
| `ret` | 160 |
| `push r` | 200 |
| `pop r` | 170 |
| `load r, [x]` (address register) | 230 |
| `load r, [k]` (constant address) | 195 |
| `store [x], s` | 260 |
| `store [k], k` | 185 |
| `switch` | 145 |

Program size (release build): `hello.mhl` 2.5 KB `.malm`; `cat.mhl` 36 KB `.malm` (1.2 M
characters of Malbolge text, 1.2 M startup steps); a program using `bz`, the byte table and the input
table about 70 KB. Each RAM word adds about 620 characters of text (and startup steps), each stack
frame 7.7 K (the default stack of 64 words about 0.5 M), each `switch` 320 K (its 256 entries);
zstd compresses them well (`.malm`: 7.5 bytes per RAM word, 210 per frame, 4 KB per switch).

## 5. Diagnostics

Errors have the assembler's form, `file:line:col: error: message` plus notes (docs/asm.md 9):
- lexical and syntax errors, one per line (the rest of the line is skipped), all lines reported;
- operands that do not fit the statement, bytes outside 0..255, numbers outside 32 bits;
- unknown names, a name of the wrong kind (register, label, RAM region), duplicate definitions;
- RAM regions of 0 words or more than 2^20 words in total, more initial values than words, a
  constant address that is not a word of a region, a stack size outside 1..65536 or declared twice,
  a switch with more than 256 labels;
- errors the assembler finds in the generated program, at the HLA statement that produced it.

## 6. Examples

`examples/hla` holds `hello.mhl` (`out "Hello, world!\n"`), `cat.mhl` (above), `exit42.mhl`
(`halt 42`) and `rev.mhl` (stdin backwards, through the stack). `tests/hla/examples_test.cpp` checks that hello and cat produce exactly the output of
`examples/asm/hello.mua` and `examples/asm/halting_cat.mua` (cat also that of the endless
`cat.mua` up to end of input; after it, `cat.mua` writes byte 168 forever while HLA's `in` reports
EOF), that `exit42.mhl` exits with 42 and that `rev.mhl` reverses its input;
`tests/hla/examples_pipe_test.sh` does the same through `malm hla` and `malm run`.

`tests/hla/control_test.cpp` has the larger programs of issue #20: a recursive factorial (with
`switch` tables for `x - 1` and `x + 1`, as there is no arithmetic yet) and a command interpreter
that dispatches with `switch` and calls handlers through a jump table in RAM, checked against a
C++ model. `tests/hla/ram_test.cpp` runs random load/store sequences against a C++ model of the
memory.
