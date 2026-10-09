# ADR 0001: 32-bit binary words and RAM access in HLA

Date: 2026-10-08
Status: accepted (spike, issue #18). Input for the HLA tickets #19–#22 and rv-emu (#25).
Decision 3 is amended by [ADR 0002](0002-hla-arithmetic.md): the add uses nibble tables.
Prototypes: [`prototypes/word32/`](../../prototypes/word32/README.md) (throwaway).

## Context

RV32IM needs 32-bit binary words: add with wrap-around, bitwise operations, shifts, byte and word
loads and stores. The machine has none of that. It has `crz` (trit-wise, spec 3.2), `rotr` over
the program width `n` (spec 3.4) and the 3-adic increment of `C` and `D`. Three more constraints
shape the design:
- **Pure loading** (spec 4). Every data cell, every table and every RAM record is written by
  startup code, at roughly 100 to 260 characters of program text and as many startup steps per
  cell (measured below and in `docs/asm.md` 7).
- **Operands are reached through D.** An `Opr` acts on the cell after the current data word. A
  register is a cell that many places must reach, so every access is a call: `MovD` to the
  register's entry, the operation, and a return through a chain of flags (docs/asm.md 3.3).
- **Tables are cheap to look up.** A `MovD` through a cell lands `D` on a table entry in O(1)
  steps, whatever the table's size. Building the index is the hard part, because there is no
  addition: two values cannot be summed into one address.

The design spec (5.2, 8) left open how to represent a word: 4 byte cells, 2 halves of 16 bits, or
native trits, with 256×256 lookup tables or trit adders. It also left open how to read RAM without
destroying it: rotate `n` times, or `crz` with a known value.

## Options

**Bits as trits.** Store each bit as one trit, `0` for 0 and `2` for 1 ("f02"). Two properties make
this the basis of all three prototyped candidates:
- `crz` works on every trit position independently, so a bitwise operation on a whole cell is one
  short, fixed sequence of `Opr`s and constant loads, whatever the width. A search over the two
  primitive moves (`prototypes/word32/search.py`) found `xor` in 10 ops and `and` in 11.
- With `A = C1`, `crz(C1, d) = d` for the trits 0 and 2, and the result goes to `A`. So one `Opr`
  reads a cell into `A` and leaves it unchanged.

The candidates differ in how many bits share a cell:

| | **A: byte cells** | **B: 16-bit halves** | **C: word cell** |
|---|---|---|---|
| cells per word | 4 | 2 | 1 |
| bit `i` of a cell | trit `4 + i` | trit `4 + i` | trit `4 + i` |
| program width `n` | 24 | 24 | 38 |

Trits 0..3 stay 0 in data. In a table index they encode the byte lane and the carry-in.

**Add.** The prototypes avoid two-operand tables. A 256×256 table indexed by `(a, b)` needs one
address built from two bytes, and that needs addition or a shift of one byte past the other.
Instead, one sequence of 9 ops computes the **column sums** `x_i + y_i` in {0, 1, 2} for every bit
position of a cell into a scratch cell `S`. Then, per byte lane:
1. A mask sequence copies the lane's 8 ternary sums into an index cell `I` (5 ops in a byte cell, 9
   in a cell with other lanes). It sets the other lanes to a constant and writes the lane number and
   the carry-in into trits 1..3.
2. A `MovD` through `I` lands on a 2-cell table entry, one of `3^8` per (lane, carry-in).
3. The entry continues in a stream for (lane, result byte, carry-out). The stream loads the merge
   constant into `A` and sets the carry flag if needed.
4. One `Opr` merges the byte into the result cell.

So the table has `2 × 3^8 = 13122` entries per lane instead of `2 × 256 × 256`, and indexing it
needs no arithmetic.

**Native trit arithmetic (rejected without a prototype).** Storing the word as a base-3 number makes
binary semantics expensive everywhere. Wrap-around at `2^32` needs a compare and subtract, and every
bitwise operation, shift and byte access needs a conversion to bits. A trit-serial adder in the
style of Nagoya's `sum`/`carry` or ELVM's HeLL backend loops over all `n` trit positions and rotates
two or three cells each time. One rotation access was measured at 21 steps (below), so the rotations
alone come to `38 × 3 × 21 ≈ 2400` steps. That is about 4× the measured table add of candidate C.

**RAM read-restore.** A RAM word is a record `[value][MB][BACK]` at an address whose trits spell
the address bits, so a pointer is built with `crz` alone (8 ops for C), with no arithmetic. An access
goes through a pointer register: `MovD` to the record, `Opr` on the value, and back through
`[record+1]`, `[record+2]`. Three read methods were measured:
- `c1`: one access with `A = C1`, for f02 values;
- `c2`: two accesses with `A = 2..2`, which swap the trits 1 and 2 twice; works for any value;
- `rot`: `n` `Rot` accesses, which rotate the cell back to its value.

## Measurements

Every operation runs as an HLA-style subroutine on fixed registers, called from a main program on 8
input pairs, including `0xFFFFFFFF + 1` and `0 + 0`. All results were correct. Steps are counted
between `Out` markers around the call, so they include the call and the return.

The first number is the cost with an ideal return. It leaves out the 3 steps of each return flag
that was tested and skipped. The number in parentheses is what the benchmark program measured, with
its own return chains. Those chains grow with the number of call sites per register, so the gap is
an overhead of the call mechanism, not of the representation.

| steps per op (ideal return, measured) | A: byte cells | B: halves | C: word cell |
|---|---|---|---|
| **xor** | 296 (427) | 152 (223) | **80** (121) |
| **add** | 888 (1641) | 786 (1533) | **663** (1392) |
| **load** a word, pointer given | 268 (801) | 138 (428) | **73** (246) |
| **store** a word, pointer given | 392 (829) | 200 (334) | **104** (154) |
| pointer from an address register | 4 × 71 (estimate) | 2 × 71 (estimate) | **71** (124) |
| read one RAM cell: `c1` (1 access) | 34 (93) | 34 (69) | 34 (57) |
| read one RAM cell: `c2` (2 accesses) | 66 (182) | 66 (134) | 66 (110) |
| read one RAM cell: `rot` (`n` accesses) | 512 (1351), `n` = 24 | 512 (1351) | 806 (2926), `n` = 38 |

The `add` of C is `9` ops of column sums, then per lane `9` mask ops, a dispatch, the stream and a
merge, then a final clean-up. That comes to about 150 steps per lane.

Code size and startup (each benchmark is the whole program: tables, registers and routines; the
tables dominate):

| | A: byte cells | B: halves | C: word cell |
|---|---|---|---|
| add-table cells (2 per entry) | 26 244 | 52 488 | 104 976 |
| program text (chars) | 7.3 M | 11.8 M | 27.4 M |
| `.malm` (zstd) | 314 KB | 491 KB | 973 KB |
| startup steps | 7.3 M | 11.8 M | 27.4 M |
| peak RSS, VM pages of `3^8` cells | 72 MB | 440 MB | 1232 MB |
| peak RSS, VM pages of `3^4` cells | 77 MB | 121 MB | 275 MB |

The VM ran at 30 to 48 M steps/s, so C's startup takes about one second.

## Decision

1. **A 32-bit word is one cell (candidate C).** Bit `i` is trit `4 + i`, stored as 0 or 2; trits
   0..3 and the head are 0. Programs that use HLA words run at width `n = 38`.
2. **Bitwise operations** are fixed `crz` sequences on the whole cell (`xor` 10 ops, `and` 11; `or`
   and the others come from the same search in #21).
3. **Add** uses column sums and per-lane `3^8` tables with a carry flag, as described above: 4 lanes
   × 2 carry-ins × `3^8` two-cell entries, plus 4 × 512 result streams. Subtraction and compares
   (#21) reuse the column-sum and table machinery with their own streams.
4. **RAM** is one record `[value][MB][BACK]` per 32-bit word. Its address spells the address bits, so
   the pointer is formed with `crz` only. Reads use `crz` with `A = C1` (one access). Writes use
   three accesses (`mov` with the RAM cell as destination).
5. **Rotate-`n` reads are not used.** At `n = 38` they cost 24× a `c1` read. `c2` is the fallback
   for values that are not f02.

Why C: it is 3.7× faster than byte cells for `xor`, 3.7× for `load`, 3.8× for `store` and 1.3× for
`add`, and it needs one pointer per word instead of four. Loads, stores, address arithmetic and
logic make up most of an RV32 instruction stream. The costs are a 3.5× larger add table (27 M
characters of program text, under 1 MB as `.malm`) and the higher `n`. Halves (B) sit between A and
C on every measure and have no advantage of their own.

## Consequences

- **Width headroom.** `n = 38` leaves one trit below the limit of 39. Values must not grow wider
  than 38 trits. Record addresses have width 37 = `n − 1`, the widest addressable (trit 36 separates
  records from the add tables).
- **VM memory.** Records spread over the address space: consecutive words are `3^6` cells apart. With
  the current `3^8`-cell pages, a 1 MiB guest RAM (262 144 records) would touch 65 536 pages, about
  3.4 GB. Pages of `3^4` cells brought the benchmark from 1232 MB down to 275 MB. Smaller pages, or
  a cell-level hash for sparse regions, are needed before rv-emu runs real programs. That belongs to
  the VM work (#37). A denser record layout would need a shift in pointer formation.
- **Startup cost of RAM.** Each record has two link cells (`MB`, `BACK`) that the startup code must
  write. For 1 MiB of guest RAM that is about 524 000 cells, tens of millions of characters and
  steps. Before #26 (emu image builder), decide between two remedies. One is to let records of
  never-written RAM rely on the fill pattern: the assembler would place trampolines where `fill + 1`
  lands, which today is in the startup area for one of the two fill values. The other is to grow the
  RAM on demand.
- **Return overhead.** The return chains added 44% to 331% in the benchmark. HLA (#19) should keep
  call sites per register low, for example ALU routines with private operand cells and copy-in /
  copy-out. A return by flag tree (O(log sites)) is worth measuring there.
- **Not measured here:** shifts, sub-word loads and stores (`lb`, `lh`, `sb`, `sh`) and compares.
  In a word cell, extracting or inserting a byte lane uses the same mask + dispatch + stream
  machinery as one lane of the add, so roughly 150 steps. A shift by `k` bits is `k` `Rot` accesses
  plus a mask, or a table per lane. #20 and #21 measure them.
- **Assembler constraints met by the prototypes.** A literal `0` cannot be the first word of a data
  line (`docs/asm.md` 6), so generated code puts register values after their entry word. Two-cycle
  `MovD`/`Opr` pairs need 17 RNops between them, so RAM access uses 4-cycles.
