# ADR 0002: The HLA arithmetic library — nibble tables, rotation loops

Date: 2026-10-09
Status: accepted (issue #21). Amends decision 3 of [ADR 0001](0001-hla-word-representation.md):
the add uses nibble tables, not byte-lane tables.

## Context

ADR 0001 fixed the word representation (one cell, bit `i` at trit `4 + i` as 0 or 2, width 38) and
designed the add as column sums plus per-byte-lane `3^8` tables: 4 lanes × 2 carry-ins × 6561
entries. Implementing #21 on top of the HLA lowering (#19, #20) measured what those tables cost
outside the prototype:

| byte-lane tables (prototype `bench_L4`, 27.4 M characters) | release | debug (ASan/UBSan) |
|---|---|---|
| `malm asm` | 4.3 s | 11 min |
| startup (27.7 M steps) | 1.1 s | 92 s |
| VM pages touched | 23 992 (about 1.2 GB) | |

Every program that adds would pay this, and every test program too: the debug build is the one CI
runs first. Two more costs showed up only in the real lowering:
- **Return trees.** A register access returns through a tree of flags over its call sites
  (docs/hla.md 4.3). A shared routine accesses its scratch cells dozens of times per call, so their
  trees grow deep, and every access pays for the depth.
- **Shifts.** ADR 0001 left them open. `Rot` is the only operation that moves trits between
  positions, one position per access, and an access through a register costs about 15 steps plus
  its return.

## Decision

1. **Nibble tables.** The add runs over 8 nibbles (4 bits) instead of 4 bytes. The index of nibble
   `k` holds its 4 column-sum digits (0..2), the other bits 1, a tag for `(k, carry-in)` in the
   trits 0..3 and the trit 36 = 2, which no other head-1 structure has. That is `8 × 2 × 81 = 1296`
   two-cell entries and `8 × 32` streams, about 1.2 M characters of program text for the whole ALU
   (54 KB `.malm`), 784 pages when it runs.
2. **One ALU routine** for `add`, `sub`, `sltu` and `slt`. The caller writes the column sums into
   the routine's cell `_AS` (5 loads and 7 accesses, the operands only read) and sets the carry flag
   for `x + ~y + 1`; the routine runs the nibbles and leaves the sum in `_AZ` and the carry-out in
   its exit. A compare is that carry. `slt` flips bit 31 of `x` around the column sums, so the top
   digit is the one of `x ^ 0x80000000` and `y ^ 0x80000000`; with a constant the top digit is
   chosen directly.
3. **A one-access mask.** Building a nibble's index reads `_AS` once, with `A = 2` on the bits,
   which swaps its digits 1 ↔ 2 instead of restoring them; odd nibbles get their own digit code.
   This saves an access and a load per nibble (about 240 steps per add) over the mask of ADR 0001.
4. **Register paths.** A register may have extra entries, each with its own return tree, chosen by
   a path flag the caller sets (docs/hla.md 4.6). The routine's own accesses to `_AS` and `_AZ` use
   one, so the program's call sites do not deepen them; each nibble has its own index register.
5. **Shifts rotate a shift register in a loop.** `_H` has a `Rot` path whose continuation rotates
   again until a 9-cycle counter (`Nop`×8 / `MovD`) fires: 15 steps per rotation, one return per 9.
   A right shift by `k` is `k` rotations, a left shift `38 - k`, then a two-access transform clears
   the trits that are not bits of the result. `sra` of a negative word shifts its complement. A
   computed amount dispatches through a switch table on its low byte.
6. **Constants never need a register.** With a constant operand the column sums (3 loads, 4
   accesses), `and`/`or`/`xor` (an in-place transform: keep, clear, fill or flip per bit) and shifts
   (an entry per amount) use per-bit constants.
7. **Sign tables** (lane indices with the trit 36 = 0, 256 two-cell entries per lane) test the top
   bit of a byte lane for `sra`, `sextb` and `sexth`.

Every sequence and table above is checked by simulation in `tests/hla/arith_word_test.cpp`, the
whole ALU exhaustively on 8-bit operands.

## Measurements

Steps per statement in a program with few call sites (`reg x, y, z`; `out` before and after):

| | steps | | steps |
|---|---|---|---|
| `add z, x, y` | 1808 | `and`/`xor z, x, y` | 117 |
| `add z, x, 1` | 1751 | `or z, x, y` | 165 |
| `sub z, x, y` | 1799 | `and`/`or`/`xor z, x, k` | 135 to 162 |
| `sltu z, x, y` | 1775 | `srl z, x, 1` / `8` / `31` | 295 / 365 / 682 |
| `slt z, x, y` | 1931 | `sll z, x, 1` / `8` / `31` | 775 / 672 / 355 |
| `slt z, x, 5` | 1715 | `sra z, x, 1` / `31` (negative) | 614 / 1001 |
| `sextb`/`sexth z, x` | 298 | `sll`/`srl`/`sra z, x, y` | 612 / 814 / 1166 |

The add is about 2.7 times the ideal 663 steps that ADR 0001 estimated for byte lanes: twice the
lanes, and real return trees. The test harness (`tests/hla/arith_test.cpp`) reports the same costs
in a program with 115 statement forms, where every register has many call sites: about 2020 for
`add`.

## Consequences

- An add costs about 1800 steps. A shared routine needs the copy-out of its result (`mov`, 84
  steps) and its call; inlining the nibbles per statement would save those, but every statement
  would carry the routine's code (about 0.1 M characters of startup) and the table streams would
  need return trees.
- Cheaper returns are the main lever left: self-toggling flags (a flag tested without its restore
  alternates by itself) could return from accesses in a fixed order without the caller setting any
  flag.
- Byte lanes stay an option for a later performance ticket if startup cost stops mattering
  (e.g. with the startup snapshot of M6); the lowering keeps the nibble layout in `word.hpp`.
