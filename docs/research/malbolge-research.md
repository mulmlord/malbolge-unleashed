# Malbolge / Malbolge Unshackled research for malbolge-unleashed

Research date: 2026-10-01. Every claim is tied to a source (link keys are defined at the bottom). Where I checked something myself (by computing it or by running the downloaded original interpreters), it is marked **[verified: ...]**. Where sources disagree or something is my inference, it is marked **UNCERTAIN**, **CONFLICT** or **INFERENCE**.

How I verified things: I downloaded the primary sources (the reference `malbolge.c`, Johansen's `Unshackled.hs`, Lutter's `Unshackled.c`/`Unshackled-20.c`, Szewczyk's `fast20.c`, Nagoya's `malbolge20.c`, `stringout.c`, LMAO and LMFAO). I compiled the C interpreters. I also wrote a throw-away Python re-implementation of the reference. With these I checked the tables, the cycle facts and the test-program outputs below.

---

## 1. Exact Malbolge semantics

### 1.1 Machine model
- Words are 10 trits wide, unsigned, range 0..59048. Memory is exactly 59049 (3^10) words. Code and data share one memory. [olm-spec]
- There are three registers: **A** (accumulator), **C** (code pointer) and **D** (data pointer). All start at 0. [olm-spec]
- C and D are both incremented after every instruction. In the reference they wrap from 59048 to 0 (`if (c == 59048) c = 0; else c++;`). [olm-ref]
- Addresses are one full word, so the whole memory is directly addressable. [eso-mb]

### 1.2 Loading (validity rule and fill rule)
- The spec says whitespace is ignored. Any other non-instruction character is an error. Each non-whitespace character is stored in one cell, starting at address 0. [olm-spec]
- The spec's validity rule: a character x at position i is valid only if `xlat1[(x - 33 + i) % 94]` is one of `j i * p < / v o`. [olm-spec] [olm-ref]
- **What the reference loader actually does** [olm-ref]:
  ```c
  if ( isspace( x ) ) continue;
  if ( x < 127 && x > 32 ) { if ( strchr( "ji*p</vo", xlat1[( x - 33 + i ) % 94] ) == NULL ) { /* reject */ } }
  ...
  mem[i++] = x;
  ```
  - Bytes outside 33..126 that are not `isspace()` are stored **without any check**. This covers 0..31 except whitespace, plus 127..255. This is the "loader bug" that Scheffer exploits. [scheffer]
  - Whitespace is whatever C `isspace()` accepts: space, `\t`, `\n`, `\v`, `\f`, `\r`.
- Allowed characters at address 0 (and every address ≡ 0 mod 94): `' ( > D Q b c u`. At address 1: `& ' = C P a b t`. [eso-mb] [verified: computed from xlat1]
- Encoding formula (instruction code k → source character at position p) [ernst-stringout] [verified for p = 0..299]:
  `ch = (k - p % 94 + 94) % 94; if (ch < 33) ch += 94;`
- **Fill rule.** Cells after the program are set to `mem[i] = op(mem[i-1], mem[i-2])` [olm-ref]. The spec only says "performing op on the previous two cells repetitively" [olm-spec].
  - Argument roles: `op(x, y)` treats x like A and y like [D] (see 1.4). So the previous cell plays the A role and the cell before it plays the [D] role. [olm-ref] [eso-mb]
  - The resulting sequence has period 6. Esolangs states this [eso-mu], and I confirmed it: for all printable seed pairs and 20k random pairs, `x[j+6] == x[j]` holds from the last loaded cell onward. [verified: Python]
- **Edge case: a program shorter than 2 characters.** The reference reads `mem[-1]`, which is undefined behaviour. The spec does not cover this. [eso-mb]
  - Unshackled.hs errors "Source program too short" [ojo-hs].
  - Unshackled.c errors "not a valid Malbolge program" [lut-uc].
  - malbolge20.c exits "empty input" [m20-c].
- Maximum program length is 59049 cells. The reference errors "input file too long". [olm-ref]

### 1.3 Execution cycle and instruction decoding
Each step [olm-ref] [eso-mb]:
1. If `mem[C]` is not in 33..126: the spec says the program "is immediately ended" [olm-spec]. **CONFLICT:** the reference does `continue` without incrementing anything, so it **hangs forever** [olm-ref] [eso-mb].
   - Unshackled.hs imitates the hang on purpose (`hang`) [ojo-hs].
   - malbolge20.c prints an error and exits [m20-c].
2. The instruction is `xlat1[(mem[C] - 33 + C) % 94]`. Equivalently, it is determined by `(C + mem[C]) % 94` [eso-mb] [verified]:

| (C+[C]) mod 94 | spec letter | reference behaviour | effect |
|---|---|---|---|
| 4  | `i` | Jmp  | `C = [D]` |
| 5  | `<` | Out  | `putc(A)` (A mod 256) |
| 23 | `/` | In   | `A = getc()`, EOF → 59048 |
| 39 | `*` | Rot  | `A = [D] = rotr([D])` |
| 40 | `j` | MovD | `D = [D]` |
| 62 | `p` | Opr  | `A = [D] = crz(A, [D])` |
| 68 | `o` | Nop  | nothing |
| 81 | `v` | Hlt  | stop |
| any other | — | Nop | nothing (only obtainable at run time, not loadable) |

3. After the instruction runs, `mem[C] = xlat2[mem[C] - 33]`, then C++ and D++ (both mod 3^10). [olm-ref]
   - Note that this uses C *after* execution. For `i` (Jmp), C has already changed, so the cell that gets encrypted is the one at the new C (the jump target minus 1). The jump instruction itself is **not** encrypted. [scheffer] [eso-mbp]
   - The spec wording: after a jump, execution continues at `[D] + 1`, because C is incremented afterwards. [jvse2010]
4. If `mem[C]` is outside 33..126 when it is about to be encrypted (because the instruction just changed it, e.g. `*`/`p` with D == C, or a jump into data), the reference indexes `xlat2` out of bounds. This is undefined behaviour. [eso-mb] [olm-ref]

### 1.4 Crazy operation (crz / "op")
Trit table from the spec, verbatim [olm-spec]:
```
            | A trit:
    ________|_0__1__2_
          0 | 1  0  0
      *D  1 | 1  0  2
     trit 2 | 2  2  1
```
- Rows are the trit of [D], columns the trit of A.
- The same table appears in Esolangs [eso-mb], the MalbolgeLISP book [mlisp-book], `Unshackled.hs` (`op T0 T0 = T1; op T1 T0 = T0; ...`, first argument = A trit) [ojo-hs], and `Unshackled.c` as `OPR[a + 3*d] = {1,0,0, 1,0,2, 2,2,1}` [lut-uc].

Di-trit table as the reference implements it (`o[y/p9%9][x/p9%9]`, y = [D], x = A) [olm-ref]. The same table appears in the spec [olm-spec]:
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
- The reference call site is `op(a, mem[d])`.
- **[verified]** The di-trit table equals the trit table applied digit by digit (`O[d][a] = T[d%3][a%3] + 3*T[d/3][a/3]`). The reference `op()` equals the trit-wise crz on 20,000 random pairs.
- Useful identities [scheffer] [eso-mbp]:
  - `crz(A, 1111111111t)` swaps 0↔1 in A.
  - `crz(2222222222t, D)` swaps 1↔2 in D.
  - Two crz ops into a cell containing 1111111111t give a "store".
  - "Load" = set A to 2222222222t, crz, then again.

### 1.5 Rotate right
- `a = mem[d] = mem[d] / 3 + mem[d] % 3 * 19683;` — the least significant trit becomes the most significant; all other trits move down one position. [olm-ref] [olm-spec]
- Both 1111111111t and 2222222222t are fixed points. [verified]

### 1.6 Encryption / xlat tables (exact, copied from the reference source)
From [olm-ref], C string literals:
```c
const char xlat1[] =
  "+b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA\"lI"
  ".v%{gJh4G\\-=O@5`_3i<?Z';FNQuY]szf$!BS/|t:Pn6^Ha";

const char xlat2[] =
  "5z]&gqtyfr$(we4{WP)H-Zn,[%\\3dL+Q;>U!pJS72FhOA1C"
  "B6v^=I_0/8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G\"i@";
```
The same strings unescaped (94 characters each; index 0 corresponds to character 33 `!`):
```
xlat1: +b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA"lI.v%{gJh4G\-=O@5`_3i<?Z';FNQuY]szf$!BS/|t:Pn6^Ha
xlat2: 5z]&gqtyfr$(we4{WP)H-Zn,[%\3dL+Q;>U!pJS72FhOA1CB6v^=I_0/8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G"i@
```
- **Verified identical** in: the spec HTML [olm-spec] (the HTML adds a stray trailing `j` after the xlat2 block because of the following `j` heading), the Esolangs translation table [eso-mb], `Unshackled.hs` [ojo-hs], `Unshackled.c` [lut-uc], `Unshackled-20.c` [lut-u20], `malbolge20.c` [m20-c], `stringout.c` [ernst-stringout], `fast20.c` (padded with junk around it: `"SOMEBODY MAKE ME FEEL ALIVE[hj9>,"` + xlat2 + `"AND SHATTER ME"`, indexed by raw value) [fast20], and mtve's `malbolge.pl` [esofiles].
- **[verified]** Both strings are permutations of the 94 characters 33..126.
- xlat2 cycle lengths are **2, 4, 5, 6, 9, 68**, matching Scheffer's list [scheffer] [verified].
  - The short cycles: `FJ`, `*r}i`, `` )f`<3 ``, `%guox:`, `2PB>LOCUI`.
- **Two-cycles.** The only 2-cycle is `F`(70)↔`J`(74), and it is address-independent [eso-mbp] [verified]. At the following addresses mod 94, one element of the FJ pair decodes to the instruction and the other to a Nop [verified]:
  - `<` at 25/29, `/` at 43/47, `*` at 59/63, `j` at 60/64, `p` at 82/86 [eso-mbp]
  - also `i` at 24/28, `v` at 7/11, `o` at 88/92 [verified; not listed on Esolangs]
- **Immutable Nops.** Addresses mod 94 where a loaded `o` stays a Nop forever: `6, 8, 10, 17, 26, 27, 37, 42, 48, 51, 82, 86, 88, 92` [eso-mbp] [verified].
  - 70/74 are immutable Nops everywhere except `7, 11, 24, 25, 28, 29, 43, 47, 59, 60, 63, 64, 82, 86` [eso-mbp] [verified].
  - Scheffer lists all instruction cycles per position [scheffer-cyc].
- Once an instruction permutes into a Jmp it stays a Jmp, because Jmp is never encrypted. [scheffer]

### 1.7 I/O
- **CONFLICT (spec vs reference): the I/O letters are swapped.**
  - The spec says `<` reads input and `/` writes output [olm-spec].
  - The reference does the opposite: `case '<': putc(a, stdout)` and `case '/': x = getc(stdin); ... if (x == EOF) a = 59048; else a = x;` [olm-ref].
  - Every later source treats the interpreter as authoritative [scheffer] [eso-mb] [jvse2010].
- **Output:** `putc(a)` is called with a 16-bit value, which C converts to unsigned char, so the byte written is A mod 256 [olm-ref] [eso-mb]. EOF written back out gives 59048 mod 256 = **168** [eso-mb]. [verified: the non-halting cat outputs byte 0xA8 after EOF]
- **Input:** the byte value 0..255 goes into A. EOF gives 59048 (2222222222t) [olm-ref]. The spec also says 2222222222t = EOF and 10 = newline [olm-spec].
- **Newline, 10 vs 13.** The reference has `#if '\n' != 10` blocks. These map the platform's `'\n'` to 10 on input and write 10 back as `'\n'` on output. They only apply on compilers where `'\n' != 10`, such as classic Mac MPW, where `'\n'` is 13. [olm-ref]
  - **Bug:** the output branch tests `x == 10`, where `x` is the last *input* character (possibly uninitialized), instead of A. [olm-ref]
  - Both the program and stdin are opened in text mode. On Windows, CRLF→LF on input and LF→CRLF on output. INFERENCE: use binary I/O and treat 10 as the canonical newline.
  - The Unshackled compatibility notes say to "accept both \10 and ...21 as newline on input" and to use `\10` on output ("may not work on Macs") [eso-mu].
- **Halting:** `v` returns from `exec`. There is no other way to halt (apart from the hang/UB cases above). [olm-ref]

---

## 2. Reference-interpreter quirks and bugs vs the spec
| # | Quirk | Source |
|---|---|---|
| 1 | I/O letters swapped (`<` = out, `/` = in in the implementation) | [olm-ref] [scheffer] [eso-mb] |
| 2 | Loader stores non-whitespace bytes outside 33..126 unchecked, so you can load 0..31 (non-space) and 127..255. Scheffer's copy/cat program `copy.mal` and his "immutable Nop via 129..255 then rotate" trick depend on this | [olm-ref] [scheffer] [esofiles] |
| 3 | Executing a cell outside 33..126: spec says "ended"; reference loops forever | [olm-spec] [olm-ref] [eso-mb] |
| 4 | Encrypting a cell outside 33..126 (after `*`/`p` hit [C], or after a jump) indexes `xlat2` out of bounds (UB). mtve's `answer.mal` relies on this: it prints `42` in the Perl implementation, which warns "attempt to exploit security hole" and treats the undefined entry as 0. With the C reference and in my strict model it produces no output | [olm-ref] [esofiles] [verified] |
| 5 | Programs of fewer than 2 characters read `mem[-1]` | [eso-mb] |
| 6 | `'\n' != 10` branch tests the wrong variable (`x`) | [olm-ref] |
| 7 | Assumes 16-bit `short`; the MSC memory-model comments | [olm-ref] |
| 8 | Spec's "full stop" / "program is immediately ended" vs the interpreter's `return` / hang semantics | [olm-spec] [olm-ref] |

The Nagoya group also states that the spec "is not complete and also has a slight mismatch with the interpreter", so "it is reasonable to follow the interpreter" [jvse2010]. Scheffer says the same [scheffer].

---

## 3. Malbolge Unshackled, Malbolge20 and what "3^n memory" means

### 3.1 Malbolge Unshackled (Ørjan Johansen, 2007) — spec [eso-mu], reference interpreter [ojo-hs]
- **Values.** Values and addresses have an unbounded number of trits. They are a subset of the 3-adic integers: the first (leading) trit repeats forever to the left, so `...01 = ...0001`.
  - Values with head 0 are the natural numbers; these are used for I/O and decoding.
  - "Width" = the number of trits other than the repeating head.
  - Reason: `crz(0,0) = 1`, so zero-padding would change results. [eso-mu]
- **Remainders (mod 94 decoding, mod 6 fill).** `...111` and `...222` behave like 1111111111t (29524) and 2222222222t (59048). Other head-1 and head-2 values count as offsets from those. Example: `...1102 mod 94 = (29524 - 2) mod 94 = 6`. [eso-mu]
  - Implementation: `mod()` in [lut-uc], `modClass` in [ojo-hs].
- **Memory initialization.** The 6-periodic Malbolge fill is extended to addresses that do not start with 0, using remainders mod 6. [eso-mu]
  - Unshackled.hs builds a 6-entry `restArray` from the last two program cells and assigns each lazily created node `restArray ! (modClass mod 6)`, with `fullMod = lcm 6 94`. [ojo-hs]
  - Unshackled.c: `initial_values[6]`, filled lazily on first access. [lut-uc]
- **Rotation width** — the core change [eso-mu]:
  - It starts at **≥ 10** trits.
  - If a `j` makes the width of D greater than half the rotation width, the rotation width grows to fix that.
  - Otherwise it does not change.
  - Whether the automatic D++ can register a width change is unspecified.
  - Growth is "fairly unpredictable (and implementation dependent)". Dropping the shifted-out trit or placing it at a fixed position would cap the size of values, which is why the width is variable instead. [eso-mu]
  - **Reference behaviour** [ojo-hs]: initial `rotWidth` is random in 10..15. The growth policy is randomly deterministic (`step` 4..12, `slack` 0..5; grows to `max(2*newMax, rot+step)` when `2*newMax+slack > rot`) or randomly probabilistic. Growth is checked only in `J` (MovD), when the new D node's width exceeds `maxWidth`.
  - `Unshackled.c` copies this (`rotwidth = 10 + rand()%6`, same two policies, seeded with `time(NULL)`; check only in `case 40`) [lut-uc].
  - Rotation in the reference: the value is head-extended to `w` trits; the low `w` trits rotate; trits above position `w` are unchanged. [ojo-hs]
  - **UNCERTAIN / possible CONFLICT:** `Unshackled.c`'s `rotate_r` rotates the whole stored trit list when the stored width is greater than `rotwidth`, rather than only the low `rotwidth` trits. The two reference implementations may disagree for values wider than the rotation width. This comes from reading the code; I did not test it. [lut-uc]
- **I/O.** Values with head 0 are Unicode code points; no mod 256. `...22` = EOF and `...21` = end-of-line, converted automatically on I/O. Heads 1/2 are reserved for future use. [eso-mu]
  - [ojo-hs]: output of `...22` closes stdout, output of `...21` prints `\n`; input `\n` → `...21`, EOF → `...22`.
  - [lut-uc]: UTF-8 in and out; invalid code point → error exit; output of EOF → error.
  - `...21` "is not possible to generate from scratch" without first reading EOL/EOF, so in practice programs print 10. [lut-uhw] [mlisp-readme]
- **Loader.** Unshackled.hs by default *allows* out-of-range bytes, like the reference; `-n` rejects them [ojo-hs]. Unshackled.c rejects anything except `' ' \t \r \n` and valid instructions [lut-uc].
- **Compatibility tips** [eso-mu]:
  - avoid `*` unless the first and last trits of [D] are equal
  - use exact I/O values
  - accept 10 and `...21` as newline
  - avoid programs longer than 1/3 of memory
  - avoid wrapping across memory thirds

  An early interpreter bug that kept the width at 10 made old Malbolge programs look compatible [eso-mu] [ojo-hs changelog].
- **How programs cope with the unknown width** (they bound it rather than detect it):
  - Without knowing the rotation width, you cannot load specific values above **3^4−1 = 80**, except a few head-1 values that are useless for I/O. So printing any character above `P` requires "a carefully designed loop" to find the width first. [lut-uhw] [mlisp-book]
  - A program can only **force a lower bound**: doing `j` to an address of width w forces rotwidth ≥ 2w. LMFAO's `init_all` does exactly this ("create ...0222222 with max 2s, crazy it into C0, MovD to target and return", repeated until `min_rot_width ≥ needed_backjump_rotwidth`). [lmfao-src]
  - Lutter's generator and LMFAO both prepend the same ~19391-character bootstrap "init module". [lut-gen] [lmfao-src]
  - MalbolgeLISP uses a "rotwidth loop" and embeds "an extension loop pass into each iteration" of arbitrary-precision arithmetic. [mlisp-book]
- **Turing completeness.** Shown by Lutter's brainfuck interpreter (2 MiB `.mu`, from HeLL via LMFAO) and later by MalbolgeLISP. [eso-mu] [lut-bf]
- **Interpreters:**
  - `Unshackled.hs` is very slow.
  - `Unshackled.c` supports rotation widths up to `UINTMAX_MAX` (trit linked lists; slow but spec-faithful).
  - `Unshackled-20.c` uses a fixed rotation width of 20: head trit ("area") + 2×10-trit halves, so memory is 3×3^20. It does not grow the width.
  - `fast20.c` (see 3.3) [lut-interp] [lut-u20].

### 3.2 Malbolge20 (Nagoya, Kato & Sakai, 2013)
- Word length and address space are 20 trits, so memory is 3^20 ≈ 3.49·10^9 cells. [eso-m20] [m20-readme] [nagoya]
- There is no separate prose spec; the interpreter source is the definition [m20-c]:
  - 59049 blocks of 59049 cells each, allocated lazily.
  - Each block's first two cells are precomputed from the previous block's last two, via a double-step crz table (`num0`/`num1`), and the rest is filled with crz.
  - Rot uses `3^19`, so the rotation width is 20. crz runs over 10 di-trits.
  - Decoding uses the full address: `(mem - 33 + c + c_id*59049) % 94`.
  - Loader is strict (rejects non-graphic non-space bytes).
  - Executing a value outside 33..126 is an error exit ("enterring into an infinite loop"), not a hang.
  - **QUIRK:** EOF is `a = 59049` (0t1 followed by ten 0s), **not** all-2s (3^20−1). This looks like a leftover from the 10-trit code. [m20-c]
  - Output is `putc(a)` (mod 256).
- Toolchain: C-like language → "pseudo-instruction language with control" (`.mg`) → LAL (`.mc`) → memory image (`.data`) → `.mb`. The `init` step accounts for more than 98% of build time (~0.39 ms per data byte), and `.mb` is ~200× the `.data` size (e.g. a 10-line `array.c` becomes a 33.7 MB `.mb`). [m20-creator]
- Szewczyk: Malbolge20 is still not Turing-complete (bounded memory), and Nagoya's addition routine truncates at 20 trits. [mlisp-book]

### 3.3 Fixed-width Unshackled (fast20) — a practical "3^n memory" model
- `fast20.c` [fast20] [mlisp-book]:
  - `typedef u32 W`, `SZ 19`, `END = 3^19`. Memory is one `mmap(PROT_NONE, MAP_NORESERVE)` region of 3^19 words.
  - A SIGSEGV handler maps pages on demand and fills them with the 6-periodic pattern `pat[(off within third) % 6]`.
  - Decode: `(c + mem[c] + offs[c/(END/3)]) % 94`, where `offs` corrects the upper two thirds (head trit 1 or 2).
  - I/O is raw bytes (`putchar(a)`, EOF = `END-1`, no `...21` newline mapping).
  - **[verified]** The third-offsets (23 and 46 mod 94) and the mod-6 fill offsets match the Unshackled remainder rule for N = 19. So the top trit acts as the repeating 3-adic head. `Unshackled-20.c` uses 52 and 10, which match N = 20 [verified].
  - **CONFLICT:** the book says fast20 has "a fixed rotation width of 19 trits", but `mrot` rotates within `END/3 = 3^18`, i.e. the low **18** trits, keeping the head trit. Earlier fixed-width interpreters used width 20. [mlisp-book] [fast20] (from reading the code)
  - Szewczyk admits fast20 "is not a valid Malbolge Unshackled interpreter" because the rotation is fixed. [mlisp-book]
- **What "3^n memory" plausibly means — INFERENCE, a design decision for this project:**
  - (a) Unshackled semantics exactly. Addresses of width w cover 3^w cells per head-region, so memory "grows in powers of 3" as programs reach wider addresses, and rotation width grows with D.
  - (b) A fixed n (Malbolge20-like, or fast20-like with head trit) plus lazily paged memory. This is effectively unbounded but capped at 3^n.
  - (c) A new rule where word width n (= rotation width = log3 of memory) steps n → n+1 on some trigger.
  - Option (c) is only well-defined if values are head-extended (3-adic) as in Unshackled. With zero-extension, crz results already stored would no longer be consistent with (n+1)-trit arithmetic, because crz(0,0) = 1 [eso-mu]. It also needs a trigger: with fixed n-trit arithmetic no value can ever exceed 3^n−1, so nothing forces growth.
- **[verified] Classic programs are width-sensitive.** Cooke's `HEllO WORld` prints garbage under malbolge20 (20 trits), Unshackled.c, Unshackled-20 and fast20. Iizawa's 99-bottles fails under Unshackled-20 and fast20.
  - Lutter's `hello-world.mu`, which was designed for both, works under the reference, Unshackled.c and Unshackled-20, but fails on fast20 and malbolge20. Lutter's halting `cat.mal` produces no output under Unshackled.c.
  - Conclusion: backward compatibility with 10-trit Malbolge must be an explicit design choice.

---

## 4. Techniques for generating Malbolge

### 4.1 Normalized Malbolge
- Coined by Andrew Cooke: undo the position-dependent xlat1 mapping, so a program is written only in `j i * p < / v o`. Denormalize by inverting xlat1 at each position. [cooke] [eso-mb]
- Encoder/decoder code: `decodeInt = xlat1[(code-33+pos)%94]` and `encodeInt` [mlisp-book], plus the formula in 1.2.
- Normalized code compresses far better than raw code: entropy drops from 6.55 to 1.89 bits/byte on MalbolgeLISP. [mlisp-book]

### 4.2 Text-printing generators (search-based)
- **Cooke (≈2000).** Beam search (Lisp, based on Norvig) over normalized programs. It branches on each new memory fetch, scores prefix matches of the output, and used a beam of 10,000 to find `HEllO WORld`. [cooke]
- **Ernst `stringout.c` (2012/2015), deterministic, used for classic Malbolge** [ernst-stringout]:
  - Cell 0 = `b`, which makes a Jmp to 98. Cells 1..98 are Nops, execution continues at C = 99 with D = C − 98, so D walks over the already-initialized low memory.
  - For each output character, it searches for the shortest window of the form `Nop* Rot? Nop* Opr? Nop* Opr` (or `Nop* Rot`) that yields A ≡ char (mod 256) using the *runtime* memory values. It then emits `Out`, and finally `Hlt`.
  - Characters 154..208 cannot be generated this way. Practical limit is ~2000 characters.
  - [verified] Its published output prints `Hello World!\n`.
- **Lutter `generator.c` for Unshackled** (2017, GPLv3): prepends the 19391-character init module, handles rotation-width growth (`inc_min_rotwidth`) and prints UTF-8 text. [lut-gen]
- **Scheffer loader idea:** a small bootstrap that reads bytes and uses an "idler" cell (22222xxxxx / 11111xxxxx) with Opr/Rot to set arbitrary ADDR/DATA values. This gets around the "only 8 characters are loadable" restriction. [scheffer-loader]

### 4.3 Control flow: how loops and branches are built
- Jmp is not encrypted, but the cell *before* the target is. So "branch to a branch" pre- or post-fixes code, and executing `X` followed by a jump to `X+1` (Scheffer's "restore") cycles 2-cycle instructions back to their original state. [scheffer] [eso-mbp]
- Place each instruction at an address where it is in a 2-cycle with a Nop, followed by a Jmp. Then drive the program with **D as the program counter**: the data area is a list of code pointers (Iizawa's technique, used in 99 bottles). [eso-mbp] [jvse2010]
  - Branching = MovD (`j`) with a 2-cycle. A "flag" is a MovD/Nop cell whose state (restored or not) chooses the path. Multi-state flags can use longer cycles. [eso-mbp] [mlisp-book] [lal]
  - MalbolgeLISP contains ~14,000 NOP/MOVD flags. [mlisp-book]
- Nagoya LAL [lal] [jvse2010]:
  - Instructions: `ROT/OPR/JMP/OUTPUT/INPUT` plus the matching `REV …` restore, `SKIP`, `IF FLAG BRANCH`, `NEXT FLAG`, `DUP`.
  - Flags can have cycle 2, 4, 5, 6 or 9 — the xlat2 cycle lengths.
  - Fixed unit: cells 59046–59048 = 74 (Nop), cell 0 = 98 (Jmp).
  - HAL (high level): `INC X, DEC X, MOV X,Y, BRANCH X,Y (if X==0), INPUT, OUTPUT, STOP`. [jvse2010]
- **HeLL / LMAO** (Lutter/Ernst, GPLv3) [lmao] [lmao-readme]:
  - `.CODE` blocks hold xlat2 cycle descriptions (`Opr/Nop` then `Jmp`, `RNop` = immutable Nop).
  - `.DATA` holds the program logic (D-driven; `ENTRY` label).
  - `R_` = label+1 (restore); `U_` = auto-insert RNops.
  - Constants: `C0, C1, C2, C20, C21, EOF`. Operators: `+ - * / >> << !`.
  - LMAO places the blocks and emits one-time straight-line initialization code for values that cannot be loaded directly. Examples ship for cat, halting cat, hello world, digital root and adder.
  - HeLL is "based on LAL". [lmao]
- **LMFAO** (HeLL → Malbolge Unshackled, v0.1.5): adds 3-adic constants (`1t00212`, `...1221`, `C12`) and rotation-width handling. [lmfao] [lmfao-src]
- **Malbolge disassembler:** Malbolge → HeLL. [lut-dis]

### 4.4 Compilers from higher-level languages
- **Scheffer, BF → Malbolge (design only)** [scheffer-bf]:
  - Model: 243 cells × values 0..242, plus an address register and an accumulator.
  - Inc/dec and conditional branch are table lookups: a Rot-to-2222222222t followed by a jump into a 243-long immutable-Nop sled that ends in Opr/Load.
  - Load/store are O(memory) sweeps.
- **Nagoya:** C-like language → Malbolge20 (`malbolge20_creator`, `mbcc` driver) [m20-creator]. There is a 2017 paper on a C subset with recursive calls, and a 2012 paper on array mechanisms [nagoya].
- **ELVM:** C → ELVM IR → HeLL backend (`target/hell.c`, contributed by Lutter) → LMFAO → Malbolge Unshackled.
  - I/O is mod 256; newline conversion "cannot be reverted in a platform independent way".
  - Not tested by default because it is "extremely slow". [elvm]
- **Lutter's BF interpreter in Unshackled:** input format is `<bf program>!<input>` (or EOF as separator). Unbounded cells to the right, arbitrary-size values, newline → 10, EOF → 0. It takes minutes, and ~15 min for the Esolangs hello world on the reference interpreter. [lut-bf]
- **MalbolgeLISP (Szewczyk)** [mlisp-book] [mlisp-readme]:
  - 21k lines of asm2bf-based source → 380k lines of low-level assembly → ~340–368 MB of Malbolge Unshackled.
  - The private toolchain is ~57k LoC and is **not published**, as far as I found (UNCERTAIN).
  - Uses bounded (2^26) arithmetic by default, plus arbitrary-precision `+'`/`-'`.
- **blc-mb:** a Binary Lambda Calculus engine in Malbolge (48 MB), run on fast20. [blc-mb]

---

## 5. Implementation strategy recommendations (these are my recommendations, based on the sources above)

### (a) Interpreter for the unbounded variant
- **Pin down the value model first.** If words may widen, use head-extended (3-adic) values as Unshackled does [eso-mu]. Otherwise crz results change meaning when the width grows.
- **Represent a word** as `u64` holding N ≤ 40 trits (3^40 ≈ 1.22·10^19 < 2^64), with the top trit acting as the repeating head (fast20 layout [fast20]). Fall back to big integers or trit vectors only beyond that.
  - Unshackled.c's per-trit linked lists are spec-faithful but slow [lut-uc]. Fixed-width representations are the known fast path [mlisp-book].
- **crz:** use lookup tables on trit chunks. For example a 9×9 di-trit table (as in the reference / fast20 `crz2`), or 3^5 = 243-wide chunk tables (243×243 u8 = 59 KB), combined by radix. The fast20 profile shows crz and JMP each at ~15% of cycles. [fast20] [mlisp-book]
- **Decode:** precompute `op_of[(c mod 94)][value]` for values 33..126 (94×94 table). Precompute `xlat2` as a 94-entry table. For upper-region addresses, add the per-region mod-94 offset (the `offs` trick) [fast20].
- **Memory:** sparse pages, lazily filled from the 6-periodic pattern with a per-region mod-6 offset. This is exact because the fill has period 6 [eso-mu] [verified].
  - Portable version: `HashMap<page_id, Box<[u64; 3^k]>>` or a two-level table.
  - Linux-only: fast20's `mmap(PROT_NONE)` + SIGSEGV trick. Note it fails on Cygwin, and was very slow in QEMU. [mlisp-book] [search result on the MalbolgeLispForensics repo; not independently verified]
- **Rotation width:** a deterministic policy (e.g. start at 10, set to `max(rot, 2*width(D))` on `j`) is still inside the Unshackled rules [eso-mu]. It makes runs reproducible, which is useful for tests. Decide whether classic 10-trit programs must keep working (see 3.3). Optionally add a `--seed` / random policy for Unshackled conformance testing.
- **Error behaviour** — define it explicitly rather than inheriting UB:
  - executing a value outside 33..126 (reference: hang; spec: stop)
  - encrypting a value outside 33..126 (reference: UB)
  - programs with fewer than 2 cells
  - loading bytes > 126 (reference: accepted)

  Offer a `--compat=reference` mode for the loader bug if 2004-era programs matter (`copy.mal`).
- **I/O:** binary mode. Classic: bytes, output mod 256, EOF = all-2s. Unshackled: Unicode/UTF-8 with `...21`/`...22`. Flush on every output or use line buffering for interactive pipelines (fast20 calls `fflush` on every output [fast20]).
- **Performance data points:** my Python model runs the 99-bottles program (11,459 bytes of output) in ~3 s; C versions are near-instant. MalbolgeLISP `(! 6)`: 8 s with new fast20 vs 25–30 s with the old one. [mlisp-book]

### (b) Generator for a text-printing program
- **Classic 10-trit target:** port Ernst's `stringout.c` algorithm (4.2). It is short and deterministic, and its output is easy to check against the reference.
- **For unbounded output or wider words**, use a HeLL-like approach:
  - D-driven data list: `ROT C2 R_ROT`, `CRAZY <const> R_CRAZY`, `OUT ?- R_OUT` [lmao-examples].
  - One `Out/Nop`+`Jmp` code unit reused via restore.
  - One-time init code to build non-loadable constants.
  - The size is then O(len(text)) with no 59049 limit.
- If the variant keeps an unknown/random rotation width, copy Lutter's bootstrap approach [lut-gen]. If rotation width is deterministic, you can skip that entirely.

### (c) Compiler from a simple source language (Brainfuck candidate)
- **Layering that has worked in practice:** source → register/VM IR (HAL-like: INC/DEC/MOV/BRANCH-zero/IN/OUT/STOP [jvse2010], or ELVM IR [elvm]) → HeLL-like D-driven assembly → layout + init-code generation (LMAO/LMFAO [lmao] [lmfao]) → `.malm`.
- **Brainfuck mapping:**
  - `+ -` → table-lookup inc/dec [scheffer-bf], or trit add as in Nagoya's `sum`/`carry` routines [mlisp-book].
  - `[ ]` → branch-if-zero via a flag or table lookup.
  - `> <` + cell access → the hard part. It needs indirect load/store, which Scheffer does in O(memory) [scheffer-bf]; Nagoya's array mechanism paper addresses the same problem [nagoya].
  - Recommendation: bound the BF tape per compile (e.g. 3^k cells), and grow it by choosing k at compile time.
- **Alternative MVP:** embed an existing BF interpreter written in Malbolge Unshackled (Lutter's `brainfuck.mu`) and feed it `program + '!' + stdin`. **UNCERTAIN license:** LMFAO and `brainfuck.hell` are GPLv3 per the pages. The `.mu`'s license is not stated. [lut-bf] [lmfao]
- **Stdin through the pipeline:** the variant's `In` instruction reads the process's stdin, so a native executable simply inherits it. For the embedded-BF approach, stdin must be prefixed with the program and a separator [lut-bf].

### (d) Packaging into a native executable
- Option 1: generate a C/Rust source file with the (normalized, compressed) program as a static byte array plus the interpreter, then compile.
- Option 2 (no compiler needed at build time): a prebuilt runner binary that reads a payload appended to itself (trailer with magic + length).
- Real programs are huge (MalbolgeLISP ~340 MB). Store them normalized and compressed: PPMd reaches a 68.5× ratio and LZMA 43.8× on normalized code [mlisp-book]. Decompress into the memory image at start-up.
- Define `.malm` to carry: word width / variant, the compat flags (loader bug, EOF/newline mode, rotation policy), and the program cells (possibly including non-printable initial values if the "loader bug" mode is enabled).

---

## 6. Test programs usable as conformance tests
Output hashes are SHA-256 prefixes, produced by the reference `malbolge.c` compiled unchanged (gcc) with empty stdin, except where noted [verified].

| Program | Source | Size | Expected (reference 10-trit) |
|---|---|---|---|
| Hello (Esolangs): `` (=<`#9]~6ZY32Vw/.R,+Op(L,+k#Gh&}Cdz@aw=;zyKw%ut4Uqp0/mlejihtfrHcbaC2^W\>Z,XW)UTSL53\HGFjW `` | [eso-hw] | 89 | `Hello, world.` then halt |
| Hello (MalbolgeLISP README) `` (=<`#9]~6ZY327Uv4-QsqpMn&+Ij"'E%e{Ab~w=_:]Kw%o44Uqp0/Q?xNvL:`H%c#DD2^WV>gY;dts76qKJImZkj `` | [mlisp-readme] | 88 | `Hello, world.` |
| Cooke `hello-world.mb` | [cooke] | 121 | `HEllO WORld` (no newline) — **fails on all wider-word interpreters** |
| Ernst stringout sample | [ernst] | 275 | `Hello World!\n` |
| Lutter `hello-world.mu` | [lut-uhw] | 24714 | `Hello, world!\n`; works in reference Malbolge, Unshackled.c and Unshackled-20 (not fast20 or malbolge20) |
| Cat, non-halting (Esolangs `...gJ%`; same text in [mlisp-book]) | [eso-mb] [lut-cat] | 66 | echoes input, then byte 168 forever after EOF. Under Unshackled.c it echoes, then exits with "invalid unicode codepoint" when writing EOF. **CONFLICT:** the MalbolgeLISP README's copy ends `...gJk` and is *rejected* by the reference loader ("invalid character"), so it is a transcription error |
| Normalized form of that cat: `jpoo*pjoooop*ojoopoo*ojoooooppjoivvv o/i <iviv i<vvvvvvvvvvvvv oji` | [eso-mb] | — | round-trip test for normalizer |
| Lutter `cat.mal` (halts on EOF) | [lut-cat] | 9503 | exact echo, then halt (also halts on empty input) |
| Lutter `cat.mu` (Unshackled, halting) | [lut-ucat] | 211681 | exact echo (tested with Unshackled.c) |
| Truth-machine | [eso-tm] | 254 | input `0` → `0` halt; input `1` → `1` forever |
| Iizawa 99 bottles (real loops) | [b99] | 22806 (program part of page) | 11,459 bytes, song from 99 down to "No more bottles…", sha256 `a759597138f098c0…`; fails on Unshackled-20/fast20 |
| esofiles `beer.mal` (straight-line; prints a uuencoded gzip of the lyrics) | [esofiles] | 7871 | 1175 bytes starting `begin 644 /dev/stdout` |
| Lutter quine `quine.mal` | [lut-quine] | 59852 | output == source (sha256 `dca8476f8b70c846…` for both) |
| Scheffer `copy.mal` (needs loader bug, non-printables) | [esofiles] [scheffer] | 441 | echoes, then 168 forever — tests the `--compat=reference` loader |
| mtve `answer.mal` | [esofiles] | 29 | **UB-dependent**: `42` only in the Perl implementation; no output with the C reference. Use as a negative test |
| Malbolge20 `sample/hello20.mb` | [m20-repo] | 188207 | `HelloWorld` under malbolge20; rejected by 10-trit ("too long") |
| Lutter `brainfuck.mu` | [lut-bf] | ~2 MiB | e.g. input `,[.,]!bf` → `bf` (Unshackled; slow) |
| MalbolgeLISP, blc-mb | [mlisp-repo] [blc-mb] | 48–350 MB | stress / performance tests (fast20 semantics) |

Other collections: [esofiles] (`nop.mal` by Olmstead, the Perl/C interpreters, `Unshackled.hs`); the Lutter/malbolge.org program pages (digital root, adder, quine) [lut-mb]; Nagoya ROT13 [nagoya].

---

## Open design questions for malbolge-unleashed (from the above)
1. **Value model:** Unshackled 3-adic head-extended values (needed for consistent growth), or a fixed width n?
2. **What triggers growth:** D width (Unshackled), C/D increment past 3^n−1 (which breaks programs relying on the reference's wrap-to-0), or something explicit?
3. **Rotation width:** deterministic (reproducible, simpler generator) or Unshackled-style unknown/random (spec-faithful, forces the bootstrap loops)? Is rotation width equal to word width?
4. **Backward compatibility:** must classic 10-trit Malbolge programs (Cooke, 99 bottles) run unchanged? Verified: they break under wider words.
5. **I/O:** bytes mod 256 (Malbolge) or Unicode + `...21`/`...22` (Unshackled)? What are the EOF and newline values at arbitrary width (all-2s, cf. Malbolge20's 59049 anomaly)?
6. **Undefined cases:** loader bug, out-of-range execute/encrypt, programs shorter than 2 cells.
7. **`.malm` format:** raw vs normalized+compressed; which metadata (variant, width, flags).
8. **BF compiler:** native codegen (hard: indirect addressing) vs embedding an interpreter (license question for `brainfuck.mu`).

---

## Sources
- [olm-spec] Ben Olmstead, Malbolge spec (1998), mirror: https://www.lscheffer.com/malbolge_spec.html — original site archived: http://web.archive.org/web/20000815230017/http:/www.mines.edu/students/b/bolmstea/malbolge/
- [olm-ref] Ben Olmstead, reference interpreter `malbolge.c`, mirror: https://www.lscheffer.com/malbolge_interp.html (also https://github.com/graue/esofiles/blob/master/malbolge/impl/malbolge.c)
- [scheffer] Lou Scheffer, "Programming in Malbolge" (cryptanalysis): https://www.lscheffer.com/malbolge.shtml
- [scheffer-cyc] Scheffer, instruction cycles: https://www.lscheffer.com/malbolge_cycle.html (legal instructions: https://www.lscheffer.com/malbolge_legal.html)
- [scheffer-bf] Scheffer, BF→Malbolge compiler outline: https://www.lscheffer.com/bf2malbolge.html
- [scheffer-loader] Scheffer, loader: https://www.lscheffer.com/loader.html
- [eso-mb] Esolangs, Malbolge: https://esolangs.org/wiki/Malbolge
- [eso-mbp] Esolangs, Malbolge programming: https://esolangs.org/wiki/Malbolge_programming
- [eso-mu] Esolangs, Malbolge Unshackled: https://esolangs.org/wiki/Malbolge_Unshackled
- [eso-m20] Esolangs, Malbolge20: https://esolangs.org/wiki/Malbolge20
- [eso-hw] Esolangs, Hello world (H–M): https://esolangs.org/wiki/Hello_world_program_in_esoteric_languages_(H-M)
- [eso-tm] Esolangs, Truth-machine: https://esolangs.org/wiki/Truth-machine
- [ojo-hs] Ørjan Johansen, `Unshackled.hs`: http://oerjan.nvg.org/esoteric/Unshackled.hs
- [lut-mb] Matthias Lutter, Malbolge pages: https://lutter.cc/malbolge/ (redirects to https://malbolge.org/)
- [lut-u] Lutter, Malbolge Unshackled index: https://lutter.cc/unshackled/
- [lut-interp] Lutter, Unshackled interpreters: https://lutter.cc/unshackled/interpreter.html
- [lut-uc] `Unshackled.c`: https://lutter.cc/unshackled/Unshackled.c
- [lut-u20] `Unshackled-20.c`: https://lutter.cc/unshackled/Unshackled-20.c
- [lut-uhw] Unshackled hello world: https://lutter.cc/unshackled/hello-world.html , https://lutter.cc/unshackled/hello-world.mu
- [lut-gen] Unshackled generator: https://lutter.cc/unshackled/generator.html , https://lutter.cc/unshackled/generator.c
- [lut-ucat] https://lutter.cc/unshackled/cat.html , https://lutter.cc/unshackled/cat.mu
- [lut-bf] BF interpreter in Unshackled: https://lutter.cc/unshackled/brainfuck.html
- [lut-cat] https://lutter.cc/malbolge/cat.html , https://lutter.cc/malbolge/cat.mal
- [lut-quine] https://lutter.cc/malbolge/quine.mal
- [lut-dis] https://lutter.cc/malbolge/disassembler.html
- [lmao] LMAO / HeLL: https://lutter.cc/malbolge/assembler.html (source https://lutter.cc/malbolge/lmao-0.5.6b.tar.gz)
- [lmao-readme] / [lmao-examples] README and `example_*.hell` inside the LMAO 0.5.6b tarball (above)
- [lmfao] LMFAO: https://lutter.cc/unshackled/assembler.html (source https://lutter.cc/unshackled/lmfao-0.1.5.tar.gz)
- [lmfao-src] `src/highlevel_initializer.c` in the LMFAO 0.1.5 tarball (above)
- [ernst] Matthias Ernst Malbolge page (archived): https://web.archive.org/web/20200625001041/http://matthias-ernst.eu/malbolge.html
- [ernst-stringout] `stringout.c`: https://web.archive.org/web/20200625001041/http://matthias-ernst.eu/malbolge/stringout.c
- [cooke] Andrew Cooke, Malbolge hello world (archived): https://web.archive.org/web/20200625001041/http://www.acooke.org/malbolge.html , program https://web.archive.org/web/20200625001041id_/http://www.acooke.org/hello-world.mb
- [b99] Hisashi Iizawa, 99 bottles (real loop): https://99-bottles-of-beer.spielmannspiel.com/language-malbolge-995.html
- [esofiles] Esoteric File Archive, malbolge: https://github.com/graue/esofiles/tree/master/malbolge
- [nagoya] Sakai lab / Malbolge20 project: https://malbolge20.gitlab.io/index-en.html (formerly https://www.trs.css.i.nagoya-u.ac.jp/projects/Malbolge/)
- [lal] LAL definition: https://malbolge20.gitlab.io/lal/lal-def-en.html
- [jvse2010] M. Sakai, "Introduction to Esoteric Language Malbolge", JVSE 2010: https://malbolge20.gitlab.io/papers/JVSE2010-Malbolge.pdf
- [m20-repo] / [m20-readme] / [m20-c] Malbolge20 interpreter: https://git.trs.css.i.nagoya-u.ac.jp/malbolge/malbolge20-interpreter (`malbolge20.c`, `README.md`, `sample/hello20.mb`)
- [m20-creator] Malbolge20 compiler toolchain: https://gitlab.com/malbolge20/malbolge20_creator
- [mlisp-repo] / [mlisp-readme] MalbolgeLISP: https://github.com/iczelia/malbolge-lisp (formerly kspalaiologos/malbolge-lisp)
- [fast20] https://github.com/iczelia/malbolge-lisp/blob/master/fast20.c
- [mlisp-book] K. Szewczyk, "MalbolgeLISP: Design and Implementation…": https://github.com/iczelia/malbolge-lisp/raw/master/MalbolgeLisp.pdf
- [blc-mb] https://github.com/iczelia/blc-mb
- [elvm] ELVM (HeLL backend): https://github.com/shinh/elvm (README §HeLL, `target/hell.c`)

Notes on names and sources:
- **INFERENCE:** "Matthias Ernst" and "Matthias Lutter" appear to be the same author. `Unshackled-20.c` is signed "Matthias Lutter" but points to matthias-ernst.eu; LMAO is © Matthias Lutter while Esolangs calls it "Matthias Ernst's LMAO".
- I found no "Lutz Ullrich" associated with this work.
- The Iizawa et al. 2005 paper (Japanese, IEICE SS2005-22) and the Nagasaka 2010 Turing-completeness paper are listed on [nagoya]. I did not read them; I used the English JVSE 2010 overview instead.
