# ADR 0003: HLA multiplication and division — shift-and-add and restoring division in loops

Date: 2026-10-10
Status: accepted (issue #22). Builds on [ADR 0002](0002-hla-arithmetic.md); adds a third family of
return flags to docs/hla.md 4.3.

## Context

RV32M needs `mul`, `mulh`, `mulhsu`, `mulhu`, `div`, `divu`, `rem` and `remu` with RISC-V's results
for division by zero (`x / 0 = -1`, `x % 0 = x`) and overflow (`INT_MIN / -1 = INT_MIN`,
`INT_MIN % -1 = 0`). The arithmetic library of ADR 0002 provides the building blocks and their
costs:
- the ALU (`x + y + cin` with carry-out) at about 1800 steps per call;
- rotations through a register's `Rot` path at about 15 steps each, so a shift right by 1 is
  cheap and a shift left by 1 (37 rotations) costs about 565 steps;
- in-place transforms (keep, clear, fill or flip per trit) at 4 or 5 loads and accesses;
- sign tables, which test the top bit of a byte lane.

Tables of products or quotients do not fit: a 32-bit product has no positional decomposition into
column sums of digits 0..2, and per-nibble partial products would need precomputed multiples and
more adds than they save.

## Decision

1. **Two shared routines, `_MUL` and `_DIV`, each a loop of 32 passes**, unsigned: shift-and-add
   for the 64-bit product, restoring division for quotient and remainder. A statement moves its
   operands into the routine's registers, calls it and moves the result out; the signed variants
   are handled around the call (6). The routines are paid once per program, like the ALU.
2. **Loop counters are code cells.** A pass ends with three counters `_L4A`, `_L4B` and `_L2`
   (`Nop`×3 / `MovD` and `Nop` / `MovD`, xlat2 cycles of length 4 and 2): `_L4A` fires on every
   4th pass and advances `_L4B`, which advances `_L2`; the loop ends when all three fire, after
   4 × 4 × 2 = 32 passes, with every counter back at its start. A pass costs a few steps for this,
   where a counter register would cost a rotation and a table dispatch. The loop body uses the
   same constants on every pass, so the operands move past fixed bit positions instead.
3. **Bit tables.** The loops test single bits: the lane index sequence with a mask of 1s but a 0
   on bit `i`, the trit 37 and the head gives an index with every trit 1 but bit `i` (its value)
   and the trit 37 and head (0). Only bit `i` is read, so the cell may hold stray trits outside its
   bits (a word in the middle of a shift). An entry per bit tested, one shared entry for every set
   bit. The tables lie in head 0, far above the program text (code addresses have the trit 36 = 2):
   in head 1 every 4-trit tag left over collides with the second cells of the zero and sign table
   entries (tag 14 plus 2 is tag 15 with a 2 in trit 0), and a distinguishing trit 37 would make
   the address width 38, which `n = 38` cannot address.
4. **Multiplication.** `_MX = x`, `_MY = y`, `_MP = 0`. A pass: if bit 0 of `_MY` is set,
   `_MP += _MX` (column sums, the ALU, `mov`), and on a carry fill the trit 36 of `_MP`; bit 0 of
   `_MP` goes to the trit 36 of `_MY` (a bit test and a transform; both bits 0 are cleared); both
   are rotated right once, so the trits 36 become bits 31. After 32 passes `_MP:_MY` is the
   product: `_MY` low, `_MP` high.
5. **Division.** `_DX = x`, `_DY = y`, `_DR = 0`. A pass: bit 31 of `_DR` is tested (set: cleared,
   and the flag `_FO` set), bit 31 of `_DX` moves to the trit 3 of `_DR`; both are rotated left
   once, so the trit 3 becomes bit 0. The ALU computes `_DR - _DY`; on a carry (`_DR >= _DY`) or
   with `_FO` (the remainder had 33 bits, so it exceeds `_DY`), `_DR` takes the difference and bit
   0 of `_DX` is set. After 32 passes `_DX` is the quotient and `_DR` the remainder. With `y = 0`
   every pass subtracts 0: the quotient is all ones and the remainder `x`, as RISC-V wants.
6. **Signs around the call.** `mulh` and `mulhsu` correct the unsigned high word:
   `mulh(x, y) = mulhu(x, y) - (x < 0 ? y : 0) - (y < 0 ? x : 0)`, `mulhsu` the first term only,
   each a sign test (the lane-3 sign table of ADR 0002) and a subtraction. `div` and `rem` divide
   `|x|` by `|y|` (negations through the ALU); the remainder takes the sign of `x`; the quotient is
   negated if the signs differ, unless its bit 31 is set. A quotient of `|x| / |y|` has bit 31 set
   only for `y = 0` (all ones: the result stays `-1`) and for `x = INT_MIN`, `y = ±1` (`INT_MIN`,
   its own negation), so both special cases come out of the same rule. Signs of constants are
   resolved at compile time.
7. **A third flag family.** The routines call the ALU and the bit tables, whose return trees use
   the flags `_SFk`; flags set by a caller stay set while the callee runs, so `_MUL` and `_DIV`
   return through flags of their own, `_MFk`.

Every piece is checked: the bit index sequence on cells with stray trits
(`tests/hla/word_test.cpp`), both loops on cells with the ALU model against the C++ results
(`tests/hla/arith_word_test.cpp`), every statement form in the VM against RV32M references
including division by zero and `INT_MIN / -1` (`tests/hla/arith_test.cpp`; slow: exhaustive on
sign-extended 8-bit operands and 10 000 random cases per operation).

## Measurements

Steps per statement in a small program (`reg x, y, z`; `out` before and after; release build):

| | steps | | steps |
|---|---|---|---|
| `mul z, x, y` (`y` = 0) | 13 928 | `divu`/`remu z, x, y` | 108 060 |
| `mul z, x, y` (`y` = 0xFFFF) | 44 866 | `divu z, x, y` (`y` = 0) | 111 885 |
| `mul z, x, y` (`y` = 0xFFFFFFFF) | 75 447 | `div z, x, y` (`x`, `y` positive) | 111 913 |
| `mul z, x, 10` | 18 777 | `div z, x, y` (`x` negative) | 115 776 |
| `mulhu z, x, y` (`y` = 0xFFFF) | 45 052 | `rem z, x, y` (`x` negative) | 114 562 |
| `mulh z, x, y` (both negative) | 50 740 | `div z, x, 10` (`x` negative) | 114 505 |

A multiplication costs about 13 900 steps plus about 1 920 per set bit of `y` (one ALU call each);
a pass without an add costs about 435 steps. A division pass costs about 3 360 steps: two
37-rotations (about 1 130), the ALU (about 1 950 with the column sums) and two bit tests.

Program text: the multiply routine with its bit table adds about 0.17 M characters, the divide
routine about 0.33 M; the signed forms add the lane-3 sign table (0.13 M) and their corrections
(about 60 K characters per `div` or `rem`, 100 K per `mulh`).

## Alternatives considered

- **Unrolled loops** (per-bit constants: test bit `i` directly, set quotient bit `i` directly)
  save the operand rotations of the division (about a third of a pass) but cost about 1 M
  characters per routine.
- **Doubling with the ALU** (`r + r + bit`) instead of the left rotation of the remainder costs
  1 800 steps instead of about 600.
- **Divisor-aligned division** (compare the remainder with `y << i`, shifting the divisor right,
  which is cheap) needs to know whether `y << i` has bits above 31: a zero test of the high part
  (up to 600 steps) per pass. It would save about 20 % of a pass for more code.
- **Radix 4** for the multiplication (precomputed `3x`, 16 passes) saves about a quarter of the
  adds for random operands.

## Consequences

- A `mul` with 16 set bits in `y` costs about 25 adds' worth of steps, a division about 60.
  Programs that divide by constants (decimal output) pay the full loop; strength reduction
  (shifts for powers of 2, multiplication by reciprocals) is left to the code generators of
  M3/M4.
- The levers left are those of ADR 0002 (cheaper returns, cheaper rotations) and the
  alternatives above; the loop structure stays.
