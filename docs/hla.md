# malbolge-unleashed high-level assembly (`.mhl`)

Status: language core (issue #19), calls, the stack, jump tables and RAM (issue #20), the
arithmetic library (issue #21), multiplication and division (issue #22).
Sources:
- the assembly language it lowers to: `docs/asm.md`
- the word representation: `docs/adr/0001-hla-word-representation.md`; the arithmetic:
  `docs/adr/0002-hla-arithmetic.md`, `docs/adr/0003-hla-muldiv.md`
- the implementation: `src/hla/` (`parser`, `word`, `lower`); the command: `malm hla` (docs/cli.md)

HLA is a small register language. A program declares 32-bit registers and RAM and runs statements
on them: moves, jumps, branches on zero, byte I/O, calls and returns, a stack, jump tables, loads
and stores through address registers, and the RV32IM arithmetic: add, subtract, compare, bitwise
operations, shifts, sign extension, multiply and divide. `malm hla` lowers it to an assembly program
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
  stored like any word, but they are not numbers: a label is no operand of arithmetic, arithmetic
  on a register that holds a code address is undefined, and so is jumping through a register that
  holds anything else.
- **Arithmetic** is that of RV32IM on 32-bit words: wrap-around `add` and `sub`, signed and
  unsigned compares, bitwise operations, shifts by the low 5 bits of the amount, sign extension,
  the low and high words of products, quotients and remainders with RISC-V's results for division
  by zero and overflow (3).
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
  regions share one namespace; every name is defined once. The mnemonics (including `add`, `sub`,
  `and`, `or`, `xor`, `slt`, `sltu`, `sll`, `srl`, `sra`, `sextb`, `sexth`, `mul`, `mulh`,
  `mulhsu`, `mulhu`, `div`, `divu`, `rem`, `remu`), `reg`, `ram` and `stack` are reserved.
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
| `add d, r, s`, `add d, r, k` | `d := r + s` (or `+ k`), modulo `2^32` |
| `sub d, r, s`, `sub d, r, k` | `d := r - s`, modulo `2^32` |
| `and d, r, s`, `or ...`, `xor ...` | `d := r & s`, `r \| s`, `r ^ s` (or with `k`) |
| `slt d, r, s`, `slt d, r, k` | `d := 1` if `r < s` as signed words, else 0 |
| `sltu d, r, s`, `sltu d, r, k` | `d := 1` if `r < s` as unsigned words, else 0 |
| `sll d, r, s`, `sll d, r, n` | `d := r << (s & 31)` (or `<< n`, `n` 0..31) |
| `srl d, r, s`, `srl d, r, n` | `d := r >> (s & 31)`, zeros shifted in |
| `sra d, r, s`, `sra d, r, n` | `d := r >> (s & 31)`, copies of bit 31 shifted in |
| `sextb d, r`, `sexth d, r` | `d :=` the low 8 or 16 bits of `r`, sign-extended |
| `mul d, r, s`, `mul d, r, k` | `d :=` the low 32 bits of `r * s` (or `* k`) |
| `mulh`, `mulhsu`, `mulhu d, r, s` | `d :=` the high 32 bits of `r * s`: both signed, `r` signed and `s` unsigned, both unsigned (or with `k`) |
| `divu d, r, s`, `remu d, r, s` | `d := r / s`, `r % s` as unsigned words; `s = 0`: `0xFFFFFFFF`, `r` |
| `div d, r, s`, `rem d, r, s` | `d := r / s` (rounded towards zero), `r % s` (the sign of `r`) as signed words; `s = 0`: `-1`, `r`; `-2^31 / -1`: `-2^31`, `0` (or with `k`) |

The exit code needs `EXIT_A`: a `.malm` from `malm hla` has it; Malbolge text (`--text`) cannot carry
it, so run text with `malm run --exit-a`.

In arithmetic statements `d`, `r` and `s` may be the same registers (`add x, x, x`); the constant
`k` is a number or a RAM region (its base address), and `n` a number. Multiplication and division
are those of RV32M, including its results for division by zero and for `-2^31 / -1`, so they never
fault. There is no separate
immediate-load statement: `mov r, k` loads any 32-bit constant in one statement (RV32I's
`lui rd, imm` is `mov rd, k` with `k = imm * 4096`). `sub d, r, k` is `add d, r, -k`; `not` is
`xor d, r, -1`, `neg` is `sub d, z, r` with `z` a zero register, `zext` is `and d, r, 0xFF`
(`0xFFFF`).

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
  `_GO`, `_MB` (each `X/Nop` + `Jmp`), `_HALT` (`Hlt`), the RAM access `_RA` (4.5), the flags
  `_AFk`, `_SFk`, `_MFk`, `_DIR`, `_Pk`, `_FC`, `_FN`, `_FSL` and `_FO` (`Nop/MovD` + `Jmp`), and
  the counters `_K9` (`Nop`×8 / `MovD` + `Jmp`, 4.6), `_L4A`, `_L4B` (`Nop`×3 / `MovD` + `Jmp`) and
  `_L2` (`Nop/MovD` + `Jmp`, 4.6).
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
themselves) use `_SFk`, so the two never interfere; the multiply and divide routines (which call
the ALU and tables themselves) use `_MFk`. A caller's flags stay set while its callee runs, so a
callee only calls callees of other families. Every flag that is tested is clear afterwards.

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

### 4.6 Arithmetic

ADR 0002 has the design and its measurements; src/hla/word.hpp has the sequences (`Sequence`: loads
and accesses on numbered cells) and the table layouts, `tests/hla/arith_word_test.cpp` simulates
them.

- **Register paths.** A register may have extra entries besides `__O`, each a callee with its own
  return tree: path `p` is chosen by the flag `_Pp`, which the caller sets with the call. After the
  value, the register's block tests `_P1`, `_P2`, ... and falls through to the program's path. The
  routines below access their cells on such paths, so a program's many call sites do not deepen
  the routines' returns. A `Rot` path (entry `__R`, `U__ROT <register>`) loops: after `Rot` it
  advances the 9-cycle counter `_K9` (`Nop`×8 / `MovD`) and rotates again until it fires.
- **Bitwise operations with a register** run a fixed sequence on `r`, `s` and `d` (`and`, `xor`: 11
  and 10 steps of loads and accesses; `or`: 13), reading `r` and `s` with `A = C1` or restoring
  them. If `d` is `r` or `s` the result goes to the scratch register `_T` first.
- **Transforms.** With a constant, `and`/`or`/`xor` copy `r` to `d` and transform `d` in place:
  per bit keep, clear, fill (2) or flip, by two constants and three accesses (two loads and two
  accesses if only keep and clear).
- **Column sums.** `add`, `sub`, `slt`, `sltu` write the digits `x_i + y_i` (`add`) or
  `x_i + 1 - y_i` (the others, with a carry-in of 1) into the ALU's cell `_AS`, as `sigma(digit)`
  (`sigma` = 1, 2, 0) and 1 on the other trits: 5 loads and 7 accesses, `r` and `s` only read. With
  a constant `k` the digit of bit `i` is `x_i + e_i` for `e` = `k`, `-k` (`sub`) or `~k` (the
  compares): 3 loads, 4 accesses. `slt` compares `r ^ 0x80000000` with `s ^ 0x80000000` unsigned:
  it flips bit 31 of `r` around the sums (a transform each), or with a constant uses the top digit
  `1 - x_31 + k_31`.
- **The ALU routine** `_ALU` (exits: no carry, carry; flags `_SFk`): `_AZ := C1`; per nibble `k`
  (bits `4k .. 4k + 3`): the mask builds the nibble's index in `_AJk` from `_AS` (one load and
  access, one access to `_AS` that swaps its digits 1 ↔ 2, so odd nibbles see them swapped, one
  more access), tests the carry flag `_FC`, finishes the index with the carry's constant and moves
  `D` through `_AJk` into the **nibble table**. The entry at `nibble_index(k, cin, digits) + 1` is
  `_GO _as<k>_<t>`, `t` the digits' total `d_0 + 2 d_1 + 4 d_2 + 8 d_3 + cin`; that stream loads
  the merge constant `word((t & 15) << 4k)`, sets `_FC` if `t >= 16` and continues at `_AC<k>`,
  which merges it into `_AZ` (`crz` with `_AZ`'s 1s: a set bit becomes 2, the rest stays 1) and
  resets `_AJk`. After the last nibble `_AS := C1` and the routine exits by `_FC`.
- `add`, `sub`: the sums, the call (with `_FC` set for `x + ~y + 1`), then `mov d, _AZ` (the first
  access with `A = C1` turns the 1/2 form into the word). `sltu`, `slt`: the sums, `d` reset, the
  call with the carry set; the exits load 1 (no carry: `r < s`) or 0. `sub d, r, r`,
  `slt d, r, r` and `sltu d, r, r` are `mov d, 0`.
- **Shifts.** `mov _H, r`, then the shift routine (flags `_SFk`), then `mov d, _H`. A constant
  amount `n` calls the entry `_SR<n>` (right) or `_SL<n>` (left); a register amount builds a switch
  index from its low byte (4.4) into a switch table of its own, whose 256 entries go to `_SV<b & 31>`,
  which tests `_FSL` (set by `sll`) and goes on to the entry. An entry rotates `_H` right `n` times
  (`38 - n` for a left shift): calls of 9 rotations through the `Rot` path, the first one shortened
  by advancing `_K9` first; then `_SH_TAIL` clears the trits that are not bits of the result (a
  two-access transform whose first constant the entry loaded). `sra` first tests the sign of `_H`;
  a negative word is flipped (`~`), sets `_FN`, and the tail flips it back after the shift.
- **Sign tables** (one per byte lane in use): lane indices with the trit 36 = 0 and the tag of the
  lane's zero table (word.hpp `sign_index`), 256 entries `_GO _SGN<l>` (top bit set) or
  `_GO _SGP<l>`. `sextb`, `sexth`: `mov d, r`, the sign test of lane 0 or 1, then a transform that
  keeps the low 8 or 16 bits and fills or clears the others.
- **Bit tables** (ADR 0003): the lane index sequence with `bit_mask(i)` (word.hpp) gives an index
  with every trit 1 but bit `i` (its value), the trit 37 and the head (0), in head 0 far above the
  program text. It reads only bit `i`, so the cell may hold stray trits outside its bits. An entry
  `_GO _TB0` per bit tested, one entry `_GO _TB1` for every set bit.
- **Loops of 32 passes** end with the counters `_L4A`, `_L4B` and `_L2` (4 × 4 × 2): each fires
  into the next, and the loop continues at its start until the last one fires; all three are back
  at their start then.
- **The multiply routine** `_MUL` (flags `_MFk`; its registers `_MX`, `_MY`, `_MP` on path 1, `_MY`
  and `_MP` rotating on path 2): `_MP := 0`; per pass: if bit 0 of `_MY` is set, the column sums of
  `_MP` and `_MX`, the ALU, `mov _MP, _AZ`, and on a carry a transform fills the trit 36 of `_MP`;
  bit 0 of `_MP` moves to the trit 36 of `_MY` (a bit test and transforms that clear both bits 0);
  both are rotated right once. Then `_MY` is the low word of `x * y` and `_MP` the high word.
  `mul`, `mulhu`: `mov _MX, r`, `mov _MY, s` (or `k`), the call, `mov d, _MY` (`_MP`). `mulhsu`
  subtracts `s` from `_MP` if `r` is negative (a sign test of lane 3), `mulh` also `r` if `s` is
  negative (with a constant: if `k` is).
- **The divide routine** `_DIV` (registers `_DX`, `_DY`, `_DR`; `_DX` and `_DR` rotating):
  `_DR := 0`; per pass: bit 31 of `_DR` is tested (set: cleared, and the flag `_FO` set), bit 31
  of `_DX` moves to the trit 3 of `_DR`; both are rotated left once (37 rotations right), so the
  trit 3 becomes bit 0; the ALU computes `_DR - _DY`, and on a carry or with `_FO` (tested and
  cleared on both exits) `mov _DR, _AZ` and a transform sets bit 0 of `_DX`. Then `_DX` is the
  quotient, `_DR` the remainder; `y = 0` gives all ones and `x`. `divu`, `remu`: `mov _DX, r`,
  `mov _DY, s`, the call, `mov d, _DX` (`_DR`). `div`, `rem`: before the call `_DX` and `_DY` are
  negated (through the ALU) where `r` and `s` are negative (a constant `k` is loaded as `|k|`);
  after it `rem` negates `_DR` if `r` is negative, and `div` negates `_DX` if the signs of `r` and
  `s` differ and bit 31 of `_DX` is clear (bit 31 is set only for `s = 0` and for `-2^31 / ±1`,
  whose results need no negation).

Program text (and startup steps), on top of the program's own: the ALU (routine, 1296 table
entries, 256 streams) about 1.2 M characters, paid once by a program that uses `add`, `sub`, `slt`
or `sltu`; a constant shift amount about 30 K per amount in use; computed shift amounts 0.9 M (the
switch and all 63 entries); a sign table 130 K; the multiply routine 0.17 M and the divide routine
0.33 M characters (each with the bit table), the signed forms the sign table of lane 3 and 60 to
100 K per statement.

### 4.7 Costs

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
| `add d, r, s` / `add d, r, k` | 1810 / 1750 |
| `sub d, r, s` | 1800 |
| `sltu d, r, s` / `slt d, r, s` / `slt d, r, k` | 1775 / 1930 / 1715 |
| `and`, `xor d, r, s` / `or d, r, s` | 117 / 165 |
| `and`, `or`, `xor d, r, k` | 135 to 165 |
| `srl d, r, n` (`n` = 1, 8, 31) | 295, 365, 680 |
| `sll d, r, n` (`n` = 1, 8, 31) | 775, 670, 355 |
| `sra d, r, n` (`n` = 1, 31; negative `r`) | 615, 1000 |
| `sll` / `srl` / `sra d, r, s` | 610 / 815 / 1165 (`s` = 0x7B) |
| `sextb`, `sexth` | 300 |
| `mul d, r, s` | 13 900 + 1 920 per set bit of `s` (`s` = 0xFFFF: 44 900) |
| `mulhu` / `mulh d, r, s` (`s` = 0xFFFF; both negative) | 45 050 / 50 750 |
| `divu`, `remu d, r, s` | 108 000 (`s` = 0: 112 000) |
| `div`, `rem d, r, s` | 112 000 to 116 000 |

These are the benchmark baseline of the arithmetic library (M2); ADR 0003 has more of
multiplication and division. `tests/hla/arith_test.cpp`
reports every statement form's cost in its harness program, where every register has about a
hundred call sites; there they cost about 10% more (about 2020 for `add`).

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
- a label as an operand of arithmetic, a constant shift amount outside 0..31;
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

`tests/hla/arith_test.cpp` checks the arithmetic of issues #21 and #22 against C++ reference
functions (RV32IM, with division by zero and `-2^31 / -1`). Its
harness program has every statement form (each operation with registers, with `d` the same as an
operand, with `r = s`, with constants) as a case of a `switch`; the test's I/O hook pokes the
operands into the registers before each case and reads the results back after it. The fast case
runs every form on edge values and random words (a debug build: the core forms) and reports the
steps per form; the slow cases (label `slow`) run every operation exhaustively on 8-bit operands
(sign-extended, so every byte lane sees both signs; shift amounts all 256 bytes) and on 10 000
random 32-bit cases, on several threads; multiplication and division also run fixed edge pairs
(division by zero, `-2^31 / -1`, the signs), and a debug build runs fewer of their cases.
`tests/hla/arith_word_test.cpp` checks the sequences and tables by simulation, the whole ALU
exhaustively on 8-bit operands, and the multiply and divide loops on cells.
