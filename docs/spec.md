# malbolge-unleashed Language Specification

This document is the normative reference for the malbolge-unleashed language and its `--classic` mode. Every implementation ticket tests against it. It refines section 2 of the [design spec](superpowers/specs/2026-10-01-malbolge-unleashed-design.md) (see section 10 for the differences) and takes its tables and its classic semantics from the [Malbolge research notes](research/malbolge-research.md), cited below as `[R §x]`. The language is classic Malbolge as implemented by Olmstead's reference interpreter, lifted to unbounded 3-adic values and a memory of `3^n` cells in the style of Malbolge Unshackled. The words MUST, MUST NOT and MAY are used in the RFC 2119 sense.

## 1. Notation and values

### 1.1 Notation
- A *trit* is 0, 1 or 2. Trit `i` of a value is the coefficient of `3^i` (trit 0 is the least significant).
- `0t2101` is a finite trit string, most significant trit first. It denotes a non-negative integer (here 2*27 + 1*9 + 0*3 + 1 = 64).
- `...h` followed by trits (for example `...1102`) is a value with repeating head `h`: the first digit after the `...` is the head, and the digits after it are the low trits, most significant first. `...1102` is head 1 followed by the trits 1, 0, 2 (low trit last). `...222` is head 2 with, canonically, no low trits (a *canonical* form has no low trit equal to the head at its most significant end; every value has exactly one).
- `[x]` is the contents of the cell at address `x`. `A`, `C`, `D` are the registers.

### 1.2 Values (3-adic)
- A *value* is a head `h` in {0, 1, 2} together with finitely many low trits; every trit above the low trits equals `h` `[R §3.1]`.
- `width(v)` is the smallest `w` such that every trit at position `w` or above equals the head. So `...000` and `...222` have width 0, `...0001` has width 1, and `...1102` has width 2 (its leading 1 is part of the head).
- Two representations that differ only by extra low trits equal to the head denote the same value. Every operation below is defined on the infinite trit string, so widening a value never changes its meaning.
- A value with head 0 is a non-negative integer: the *natural* with the same trits. Naturals are the only values that may be decoded as instructions (section 5). `In` produces naturals or `...222` (section 6), and `Out` accepts a value of any head (section 6). Values with head 1 or 2 are valid as data and as addresses.
- There are 3 heads and `3^w` values of width at most `w` per head.

### 1.3 Increment
The only arithmetic is the 3-adic increment `v + 1`: add 1 at trit 0 and propagate the carry upwards through the whole infinite trit string. A carry that reaches the head turns the head into `(h+1) mod 3` and continues forever, so heads flow into each other:
- `...222 + 1 = ...000 = 0` (head 2, width 0 becomes head 0, width 0).
- `...0222 + 1 = ...01000` (head 0, low trits 1000).
- `...1022 + 1 = ...100` (head 1, low trits 00; the same value as `...1100`, whose leading low 1 is part of the head).

The increment of a value of width `w` has width at most `w+1`. No other arithmetic exists in the language: there is no addition, subtraction, comparison or multiplication of values. `crz` (section 3.2) and `rotr` (section 3.4) are the only operations that combine or change trits, and the remainder rule (section 1.4) is the only way a value is turned into an integer.

### 1.4 Remainder rule
Let `R_0 = 0`, `R_1 = 29524` (= `0t1111111111`) and `R_2 = 59048` (= `0t2222222222`). For a value `v` with head `h` and low trits `t_i` (`0 <= i < width(v)`) and a positive integer `m`:

```
v mod m := (R_h + sum over i < width(v) of (t_i - h) * 3^i) mod m        (a non-negative result)
```

- For head 0 this is the ordinary integer remainder.
- The constants `R_h` are FIXED. They are Unshackled's choice `[R §3.1]` and do not depend on `n`.
- The rule is used for every `mod` in this document, for every head: `C mod 94` (decoding), `A mod 256` (output), `x mod 6` (fill) `[R §3.1]`.
- Worked check: `...1102 mod 94 = (29524 - 2) mod 94 = 6`. (Low trits `02`: `(0-1)*3 + (2-1)*1 = -2`.)

## 2. Machine

### 2.1 State
- Registers `A`, `C`, `D` hold values and all start at 0 (`0 = ...000`).
- A memory maps every address (a value) to a cell (a value). It is defined for all values of width at most 39 (section 2.2); only cells that have been written are stored, all others hold their fill value (section 2.4).
- A size parameter `n` (section 2.2).
- A step counter, a stdin cursor and the stdout stream.

### 2.2 Memory size `n`
- `n` is an integer with `10 <= n <= 39`. The *addressable cells* are all values of width at most `n-1`: 3 heads times `3^(n-1)` = `3^n` cells.
- **Initial `n`.** With `L` the program length (section 4): `n = max(10, width(L-1) + 1)`, where `L-1` is read as a natural. So `n = 10` for `L <= 3^9 = 19683`, and every program cell is addressable from the start.
- **Growth rule.** After every completed step (section 5, step 6): `n := max(n, width(C) + 1, width(D) + 1)`. `n` never shrinks. If the new value of `n` would exceed 39, the step fails with `E_WIDTH` and `n` keeps its old value.
- There is no wrap-around. Incrementing carries into higher trits and heads as in section 1.3. At every step boundary (before step 1 and after step 6) `C` and `D` have width at most `n-1`, so they are addressable. Inside a step they can be wider: `MovD` and `Jmp` load a value of width at most `n`, and if its low `n` trits are all 2 the increment of step 5 carries into trit `n` and gives width `n+1`. Step 6 either restores the boundary invariant (by raising `n`) or fails with `E_WIDTH`. The encryption in step 4 of a `Jmp` may touch a cell at an address of width `n`; it is defined by the same rules (fill value if never written).
- **Overflow at `n = 39`.** Storage has 40 trits, so a width-40 value is not representable. If the increment of step 5 would carry into the head trit of a 39-wide register (width 40), the VM fails with `E_WIDTH`. The diagnostic then reports the register values from before the increment together with an overflow marker.
- **Invariant.** Every value held in a cell, and `A`, has width at most `n` at all times; `C` and `D` have width at most `n-1` at step boundaries (see above). (Program cells, fill values and input bytes have width at most 6; `crz` does not widen; `rotr` produces width at most `n`; `n` never shrinks.) `rotr` relies on this.
- `n` is the rotation width (section 3.4). An implementation stores values in 40 trits (trit 39 is the head), which is why `n <= 39` `[design §2.2]`.
- **Consequences for code generators (non-normative).** Because the rotation width is `n`, any program that uses a `Rot` result, or any value of width at least `n-1`, as a pointer permanently grows `n`: `rotr` of a value whose low trit differs from its head yields width `n`, a `MovD` or `Jmp` to it makes the register width `n` or `n+1`, and the growth rule then raises `n`. Repeating this ratchets `n` up to 39 and ends in `E_WIDTH`. For example `iizawa_99_clean` from the classic corpus, run in unleashed mode, starts at `n = 11` and fails with `E_WIDTH` after 4258 steps with no output. Generated code MUST only `MovD`/`Jmp` to addresses it built to be below its chosen width. This is an intentional consequence of the rules, not a defect.

### 2.3 Program cells
Cells `0 .. L-1` hold the loaded program, each a natural in 33..126 (section 4).

### 2.4 Fill of never-written cells
- Define the sequence `f` by `f(L-2) = [L-2]`, `f(L-1) = [L-1]` (the last two program cells) and, for `i >= L`, `f(i) = crz(f(i-1), f(i-2))` in the notation of section 3.2 (first argument `a`, second argument `d`): the previous cell plays the `A` role and the one before it plays the `[D]` role, as in the reference `[R §1.2]`. All `f(i)` are values of head 0 or 1 or 2 computed trit-wise by section 3.2.
- The sequence `f` has period 6 from `L-1` on: `f(i+6) = f(i)` for all `i >= L-1`. Proof: `crz` is trit-wise, so each trit position (and the head) evolves independently by `u_i = T[u_{i-2}][u_{i-1}]` from the pair `(u_{L-2}, u_{L-1})`. For each of the 9 possible starting pairs the sequence has period 6 from index `L-1` (checked exhaustively; `[R §1.2]` observed the same on random programs), hence `f(i+6) = f(i)` for every program, head included. `f(L-2)` is not part of the period in general: for the pairs `(0,0)` and `(1,1)` it differs.
- Define `P[r]` for `r = 0..5` as `f(i)` for the unique `i` in `L .. L+5` with `i mod 6 = r`.
- **Every never-written cell `x` outside `0 .. L-1`, of any head, initially holds `P[x mod 6]`**, with `x mod 6` computed by the remainder rule (section 1.4). For head-0 addresses `x >= L` this equals `f(x)`.
- This generalizes Unshackled's `restArray` `[R §3.1]` (a 6-entry array indexed by the remainder mod 6) to all heads and to a program of any length; it is not claimed to be identical to `Unshackled.hs`.
- Implementations MAY materialize cells lazily. A cell that has been written keeps its written value, even if the written value equals its fill value.

## 3. Tables and operations

### 3.1 Encryption and decoding tables
These are the reference interpreter's tables `[R §1.6]`. Both are permutations of the 94 characters 33..126; index 0 corresponds to character 33 (`!`). Verified identical in the sources listed in `[R §1.6]`.

C string literals (copied from the reference):

```c
const char xlat1[] =
  "+b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA\"lI"
  ".v%{gJh4G\\-=O@5`_3i<?Z';FNQuY]szf$!BS/|t:Pn6^Ha";

const char xlat2[] =
  "5z]&gqtyfr$(we4{WP)H-Zn,[%\\3dL+Q;>U!pJS72FhOA1C"
  "B6v^=I_0/8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G\"i@";
```

The same strings unescaped (94 characters each):

```
xlat1: +b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA"lI.v%{gJh4G\-=O@5`_3i<?Z';FNQuY]szf$!BS/|t:Pn6^Ha
xlat2: 5z]&gqtyfr$(we4{WP)H-Zn,[%\3dL+Q;>U!pJS72FhOA1CB6v^=I_0/8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G"i@
```

- `xlat1[j]` is the character at index `j`. `xlat2[j]` is likewise; the encryption of a cell with code `x` (33..126) is the code of `xlat2[x - 33]`.
- `xlat1` is used at load time (section 4) and defines the decode table of section 3.5. `xlat2` is used for the encryption after every instruction (section 5, step 4).
- `xlat2` has cycle lengths 2, 4, 5, 6, 9 and 68 `[R §1.6]`.

### 3.2 `crz` (the crazy operation)
`crz(a, d)` is applied trit-wise to all trit positions, including the head: trit `i` of the result is `T[d_i][a_i]`, and the result head is `T[head_d][head_a]`. The result width is at most `max(width(a), width(d))`. `T` is the reference trit table `[R §1.4]`. Rows are the trit of `[D]`, columns the trit of `A`:

```
            | A trit:
    ________|_0__1__2_
          0 | 1  0  0
      *D  1 | 1  0  2
     trit 2 | 2  2  1
```

For example `crz(0, 0) = ...1`: head `T[0][0] = 1`. This is why zero-extending a value would change the result, and why values are head-extended `[R §3.1]`.

The reference implements the same operation on 10-trit words with the di-trit table (rows: two trits of `[D]`, columns: two trits of `A`; `o[y/p9%9][x/p9%9]` with `y = [D]`, `x = A`). It equals the trit table applied digit by digit `[R §1.4]`:

```
      A: 00 01 02 10 11 12 20 21 22
[D] 00:  4  3  3  1  0  0  1  0  0
    01:  4  3  5  1  0  2  1  0  2
    02:  5  5  4  2  2  1  2  2  1
    10:  4  3  3  1  0  0  7  6  6
    11:  4  3  5  1  0  2  7  6  8
    12:  5  5  4  2  2  1  8  8  7
    20:  7  6  6  7  6  6  4  3  3
    21:  7  6  8  7  6  8  4  3  5
    22:  8  8  7  8  8  7  5  5  4
```

### 3.3 Increment
Defined in section 1.3; used for `C := C+1` and `D := D+1`.

### 3.4 `rotr(v, n)`
Let `t_0 .. t_{n-1}` be the trits of `v` (head-extended: `t_i` is the head for `i >= width(v)`). The result `r` has:
- `r_i = t_{i+1}` for `0 <= i < n-1`,
- `r_{n-1} = t_0`,
- `r_i = h` for `i >= n`, where `h` is the head of `v` (the head is unchanged).

So the least significant trit becomes trit `n-1` and all others move down by one. The result can have width `n` (for example `rotr(0t1, n)` has trit `n-1` equal to 1 and width `n`). The values with canonically no low trits (`...000`, `...111`, `...222`) are fixed points. The invariant of section 2.2 guarantees `width(v) <= n`.

### 3.5 Instruction codes
The instruction executed at a step is determined by `k = (C + [C]) mod 94`, where `C mod 94` follows the remainder rule and `[C]` is a natural in 33..126 at decode time. Equivalently the instruction is `xlat1[(k - 33) mod 94]` (the reference's `xlat1[(mem[C] - 33 + C) % 94]`) `[R §1.3]`.

| `k` | letter | op | effect |
|---|---|---|---|
| 4 | `i` | Jmp | `C := [D]` |
| 5 | `<` | Out | write byte `A mod 256` |
| 23 | `/` | In | `A := ` next input byte, or `...222` at EOF |
| 39 | `*` | Rot | `A := [D] := rotr([D], n)` |
| 40 | `j` | MovD | `D := [D]` |
| 62 | `p` | Opr | `A := [D] := crz(A, [D])` |
| 68 | `o` | Nop | nothing |
| 81 | `v` | Hlt | halt |
| any other | | Nop | nothing (reachable only at run time, never loadable) |

**The reference interpreter's mapping is normative**, not the original specification text, which swaps `<` and `/` (there `<` reads and `/` writes) `[R §1.7]`. This fixes the I/O letter mapping only. Compatibility with the classic corpus of `[R §6]` is guaranteed only in `--classic` mode (section 8); in unleashed mode the rotation width is `n`, and classic-era programs typically ratchet `n` up to `E_WIDTH` (section 2.2).

## 4. Loader

The loader reads the program text (bytes) and produces the cells `0 .. L-1`. It is strict and pure.

1. Whitespace bytes (space, `\t`, `\n`, `\v`, `\f`, `\r`) are skipped. They occupy no cell.
2. Every other byte `x` is stored as the next cell, at index `i` (0-based, counting stored cells only). It MUST satisfy both: `33 <= x <= 126`, and `xlat1[(x - 33 + i) mod 94]` is one of `j i * p < / v o`. Otherwise the load fails with `E_LOAD`, reporting the byte offset (0-based, in the input, counting skipped bytes) and the reason.
3. `L >= 2`. A program of fewer than 2 cells is rejected with `E_LOAD`; the reported offset is the input length. There is no fixed maximum `L` in unleashed mode, but a program whose initial `n` (section 2.2) would exceed 39, that is `L > 3^38`, is rejected with `E_LOAD`. In classic mode the maximum is 59049 (section 8).
4. The loader is pure: nothing besides the program is ever preloaded into memory. All data is built by code at run time. A `.malm` container (format out of scope here) never preloads data either.

Bytes outside 33..126 that are not whitespace are rejected. Unlike the reference interpreter they are never stored `[R §1.2, §2]`.

## 5. Execution

Registers `A`, `C`, `D` start at 0; `n` and the memory are as in section 2. Execution is a sequence of steps. A step performs the following in exactly this order:

1. **Fetch check.** If `[C]` is not a natural in 33..126 (that is: its head is not 0, or its value is below 33 or above 126), fail with `E_EXEC`.
2. **Decode.** `k = (C + [C]) mod 94`; look up the instruction in section 3.5.
3. **Execute:**
   - Jmp: `C := [D]`.
   - Out: write the byte `A mod 256` to stdout (section 6).
   - In: `A :=` the next byte of stdin (`0..255`, head 0), or `...222` if stdin is at EOF (section 6).
   - Rot: `A := [D] := rotr([D], n)`.
   - MovD: `D := [D]`.
   - Opr: `A := [D] := crz(A, [D])`.
   - Nop: nothing.
   - Hlt: halt immediately. There is no encryption, no increment and no growth rule for the halting step.
4. **Encrypt** the cell at the current `C` (after a Jmp this is the new `C`; after Rot or Opr with `D = C` it is the cell just overwritten): if `[C]` is not a natural in 33..126, fail with `E_ENCRYPT`; otherwise `[C] := xlat2[[C] - 33]` (as a character code).
5. `C := C + 1` and `D := D + 1` (3-adic increment). At `n = 39` a carry into the head trit fails with `E_WIDTH` (section 2.2).
6. **Growth rule:** `n := max(n, width(C) + 1, width(D) + 1)`; if that exceeds 39, fail with `E_WIDTH` (section 2.2).

Notes:
- Operands are read before the step writes: in MovD, `D := [D]` reads the cell before step 4 encrypts anything.
- The step counter counts steps begun. A failing step and the halting step are counted.
- The cell encrypted in step 4 is the one at the `C` that step 3 left; Jmp itself is therefore not encrypted `[R §1.3]`.

## 6. Input and output

- Input and output are raw bytes. stdin and stdout are binary: there is no newline translation, and the `'\n' != 10` branches of the reference are not reproduced.
- **In:** `A := ` the next stdin byte as a natural `0..255` (head 0). At EOF, `A := ...222` (`...222 = -1` in 3-adic reading). Further `In` instructions after EOF yield `...222` again.
- **Out:** writes the single byte `A mod 256`, using the remainder rule, for every head of `A`. In particular a head-2 value that is the EOF `...222` is written as `59048 mod 256 = 168`, the same byte classic Malbolge writes after EOF `[R §1.7]`.
- stdout is flushed before every `In`, on halt and on error.
- **I/O failures** are errors: a read error on stdin (not EOF) and a write error on stdout, including `EPIPE`, fail with `E_IO` (section 7). Implementations MUST ignore `SIGPIPE` so that `EPIPE` surfaces as `E_IO`. No other output condition can fail; there is no `E_OUTPUT`.

## 7. Errors, halting and exit status

| Error | Raised when |
|---|---|
| `E_EXEC` | step 1: `[C]` is not a natural in 33..126 |
| `E_ENCRYPT` | step 4: the cell to encrypt is not a natural in 33..126 |
| `E_WIDTH` | step 6: `n` would exceed 39; or step 5 at `n = 39`: the increment carries into the head trit (section 2.2) |
| `E_IO` | step 3 (`In`, `Out`) or a flush: stdin read error (not EOF), stdout write error including `EPIPE` |
| `E_LOAD` | the loader rejects the program (section 4); reports a byte offset and the reason, not registers |

- These errors replace the reference interpreter's hang and undefined behaviour `[R §2]`.
- `E_LOAD` is a load-time error; the others are runtime errors.
- On a runtime error the VM flushes stdout, prints a diagnostic to stderr and exits with status 1. The diagnostic names the error and reports `C`, `D`, `A`, `n` and the step count, as they are when the error is detected (for `E_WIDTH`, `n` is the value before the failed growth; for `E_EXEC`, the registers before the step). The diagnostic text is otherwise implementation-defined. For `E_LOAD` the VM runs nothing and exits with status 1.
- **Halt** exits with status 0. If the `.malm` container flag `EXIT_A` is set (the container format is defined elsewhere), the exit status is instead `A mod 256` (remainder rule, any head).

## 8. Classic mode (`--classic`)

Classic mode runs classic Malbolge as the reference interpreter does, minus its defects. It is a separate mode, not a special case of section 5 with `n = 10`.

- `n` is fixed at 10. Values are plain integers `0 .. 59048` (10 trits, no head); a cell, a register and an address are all such integers.
- `crz` and `rotr` act on 10 trits: `crz` by section 3.2 with all heads removed (equivalently the di-trit table of section 3.2), and `rotr(v) = v div 3 + (v mod 3) * 19683`. Incrementing `C` and `D` wraps: `C := (C+1) mod 59049`, same for `D`. There is no growth rule.
- `mod` is the ordinary integer remainder. Decode: `k = (C + [C]) mod 94`, with the instruction table of section 3.5.
- EOF = 59048 (`0t2222222222`). In: `A :=` byte, or 59048 at EOF. Out: write the byte `A mod 256`.
- Loader: the strict loader of section 4, with a maximum of 59049 cells (more is `E_LOAD`, reporting the byte offset of the character that would become cell 59049).
- Fill: `mem[i] = crz(mem[i-1], mem[i-2])` for `L <= i < 59049` (`mem[i-1]` plays `A`, `mem[i-2]` plays `[D]`).
- Step order, `Hlt`, errors (`E_EXEC`, `E_ENCRYPT`, `E_IO`, `E_LOAD`), diagnostics, I/O flushing and exit status are those of sections 5 to 7. `E_WIDTH` cannot occur.

Reference-interpreter behaviours that classic mode deliberately does NOT reproduce `[R §2]`:
- **Loader bug:** the reference stores non-whitespace bytes outside 33..126 without any check, so programs can load 0..31 and 127..255. Classic mode rejects them with `E_LOAD`. Programs that rely on this (Scheffer's `copy.mal`) do not run.
- **Hang:** executing a cell outside 33..126 loops forever in the reference. Classic mode fails with `E_EXEC`.
- **Undefined behaviour on encrypt:** encrypting a cell outside 33..126 indexes `xlat2` out of bounds in the reference. Classic mode fails with `E_ENCRYPT`. Programs that depend on it (`answer.mal`) produce no output.
- **The `'\n' != 10` branch:** the reference's newline mapping (which tests the wrong variable on output) is not reproduced. I/O is binary.
- Also not reproduced: text-mode file I/O, reading `mem[-1]` for programs shorter than 2 cells (`E_LOAD` instead), and the 16-bit `short` assumption.

## 9. Worked examples

All examples were derived with an executable model of this specification. Head-0 values are shown as decimal naturals.

### 9.1 A hand-traced run, unleashed mode, 7 steps

Normalized program `/j<jo*v` (In, MovD, Out, MovD, Nop, Rot, Hlt). Its loader text is `u'a%@"K`, i.e. cells `117 39 97 37 64 34 75`, so `L = 7`, initial `n = 10`. The fill values are `P[0..5] = 75, ...12021, 51, ...12011, 78, ...1021`. Standard input is the single byte `A` (65).

| step | C | D | A | `[C]` | `k` | op | effect | cell encrypted (step 4) |
|---|---|---|---|---|---|---|---|---|
| 1 | 0 | 0 | 0 | 117 | 23 | In | `A := 65` | `[0]`: 117 to 111 |
| 2 | 1 | 1 | 65 | 39 | 40 | MovD | `D := [1] = 39` | `[1]`: 39 to 116 |
| 3 | 2 | 40 | 65 | 97 | 5 | Out | writes byte 65 (`A`) | `[2]`: 97 to 46 |
| 4 | 3 | 41 | 65 | 37 | 40 | MovD | `D := [41] = P[41 mod 6] = P[5] = ...1021` | `[3]`: 37 to 103 |
| 5 | 4 | `...1022` | 65 | 64 | 68 | Nop | nothing | `[4]`: 64 to 81 |
| 6 | 5 | `...100` | 65 | 34 | 39 | Rot | `[...100]` is `P[0] = 75` (`...100 mod 6 = 29520 mod 6 = 0`); `rotr(75, 10) = 25`; `A := [...100] := 25` | `[5]`: 34 to 122 |
| 7 | 6 | `...101` | 25 | 75 | 81 | Hlt | halt | none |

Each row shows `C`, `D` and `A` at the start of the step. After steps 1 to 6, `C`, `D` are incremented: step 2 leaves `D = 39 + 1 = 40`; step 4 leaves `D = ...1021 + 1 = ...1022`; step 5 leaves `D = ...1022 + 1 = ...100` (a head-1 address); step 6 leaves `D = ...101`. Output: the single byte `A`. Final state: `A = 25`, `C = 6`, `D = ...101`, `n = 10`, 7 steps, exit status 0. Step 6 shows a fill cell in a head-1 region, and `rotr` of `0t2210` (75) giving `0t221` (25).

### 9.2 A growth event

Normalized program `j*jjv`, loader text `(&&%M`, cells `40 38 38 37 77`, `L = 5`, `n = 10`. Fill: `P[0..5] = 40, ...12202, 77, ...101, 77, ...12202`.

| step | C | D | op | effect |
|---|---|---|---|---|
| 1 | 0 | 0 | MovD | `D := [0] = 40`; then `D = 41` |
| 2 | 1 | 41 | Rot | `[41] = P[5] = ...12202`; `rotr(...12202, 10) = ...12111111220` (head 1, low trits `2111111220`, width 10); `A := [41] :=` that value; then `D = 42` |
| 3 | 2 | 42 | MovD | `D := [42] = P[0] = 40`; then `D = 41` |
| 4 | 3 | 41 | MovD | `D := [41] = ...12111111220`; then `D := D + 1 = ...12111111221` |

At the end of step 4, before the growth rule: `n = 10` (addressable cells: width at most 9, `3^10` cells), `C = 4 = 0t11` (width 2), `D = ...12111111221` (width 10). Growth rule: `n := max(10, width(C)+1, width(D)+1) = max(10, 3, 11) = 11`. After: `n = 11` (width at most 10, `3^11 = 177147` cells), `D` is now addressable, and the rotation width is 11. Step 5 (`Hlt`) halts. In general, a step that leaves `C` or `D` with width 12 makes `n` at least 13.

### 9.3 The remainder rule for heads 1 and 2

- Head 1: `v = ...1102` (head 1, trits 1, 0, 2: width 2, low trits `02`). `v mod 94 = (29524 + (0-1)*3 + (2-1)*1) mod 94 = 29522 mod 94 = 6` (agrees with `[R §3.1]`). `v mod 6 = 29522 mod 6 = 2`.
- Head 2: `w = ...2101` (head 2, low trits `101`, width 3). `w mod 94 = (59048 + (1-2)*1 + (0-2)*3 + (1-2)*9) mod 94 = 59032 mod 94 = 0`. `w mod 6 = 59032 mod 6 = 4`.
- Constants alone: `...1 mod 94 = 29524 mod 94 = 8`, `...1 mod 6 = 4`; `...222 mod 94 = 59048 mod 94 = 16`, `...222 mod 6 = 2`, `...222 mod 256 = 168`.

### 9.4 Output of a head-2 value

Normalized program `/<v`, loader text `ubO`, cells `117 98 79`, empty stdin. Step 1: `In` at EOF, `A := ...222`. Step 2: `Out` writes `A mod 256 = 59048 mod 256 = 168` (byte `0xA8`). Step 3: `Hlt`. The program outputs the single byte `0xA8` and exits with status 0.

### 9.5 Fill values

Take the 2-cell program `jv`, loader text `(P`, cells `40 80`, so `L = 2`:
- `f(0) = 40 = 0t1111`, `f(1) = 80 = 0t2222`.
- `f(2) = crz(80, 40) = ...12222`: low trits `T[1][2] = 2`, head `T[0][0] = 1`.
- `f(3) = crz(...12222, 80) = 40`: low trits `T[2][2] = 1`; trits above, `T[0][1] = 0`.
- `f(4) = crz(40, ...12222) = ...12222`, `f(5) = crz(...12222, 40) = 80`, `f(6) = crz(80, ...12222) = ...1`, `f(7) = crz(...1, 80) = 80 = f(1)`: the period is 6 from `L-1 = 1` on.
- `P[0] = f(6) = ...1`, `P[1] = f(7) = 80`, `P[2] = f(2) = ...12222`, `P[3] = f(3) = 40`, `P[4] = f(4) = ...12222`, `P[5] = f(5) = 80`.

Fill of a head-1 address: `x = ...1022` (head 1, low trits `022`) has `x mod 6 = (29524 - 9 + 3 + 1) mod 6 = 29519 mod 6 = 5`, so before any write `[...1022] = P[5] = 80`. Fill of a head-0 address beyond the program: `[1000] = P[1000 mod 6] = P[4] = ...12222`.

## 10. Changes from the design spec

This document supersedes `docs/superpowers/specs/2026-10-01-malbolge-unleashed-design.md` section 2 where they differ. The differences, all deliberate:

1. **Output rule.** The design said: head 0 writes `A mod 256`, head 2 writes the negative integer mod 256, head 1 is the runtime error `E_OUTPUT`. This document replaces it by: `Out` writes `A mod 256` per the remainder rule (section 1.4) for every head. `E_OUTPUT` no longer exists. For example, the EOF value `...222` written back out is now byte 168, as in classic Malbolge, not 255.
2. **`E_IO` (new).** Stdin read errors and stdout write errors, including `EPIPE`, are the runtime error `E_IO` (section 6). `E_LOAD` is listed separately from the runtime errors.
3. **Initial `n` (refinement).** The design said `n` starts at 10. A program with more than `3^9` cells does not fit into the addressable cells of `n = 10`, so the initial `n` is `max(10, width(L-1) + 1)` (section 2.2).
4. **Fill (clarification).** The fill rule is stated exactly (section 2.4): the sequence `f`, the table `P[0..5]` and the use of the remainder rule for all heads. It generalizes Unshackled's `restArray`.
5. **Remainder constants.** `R_1` and `R_2` are fixed at 29524 and 59048 for every `n` (section 1.4).
6. **Classic mode** is specified in full in section 8, including the fill, the loader maximum and the list of reference behaviours that are not reproduced.
