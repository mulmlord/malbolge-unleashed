# HeLL / LMAO / LMFAO research notes

Purpose: a precise, sourced reference of HeLL (Matthias Lutter's Malbolge assembly language) and the LMAO / LMFAO assemblers, as input for the syntax spec of our own assembler. Everything below is described in my own words. LMAO, LMFAO and `brainfuck.hell` are GPLv3. No code or whole example programs are copied; only single tokens and grammar fragments are quoted.

Date of research: 2026-10-02.

## 0. Sources and method

Citation tags used below. Tarball tags name the file inside the tarball after the colon, for example `[lmao-tgz: src/lmao.y]`.

| Tag | URL |
|---|---|
| `[lmao-page]` | https://lutter.cc/malbolge/assembler.html |
| `[lmao-tgz]` | https://lutter.cc/malbolge/lmao-0.5.6b.tar.gz (LMAO v0.5.6b: `README`, `CHANGELOG`, `datamodule.txt`, `example_*.hell`, `src/*`) |
| `[tut]` | https://lutter.cc/malbolge/tutorial/cat.html (Lutter's beginner tutorial, a HeLL cat walk-through) |
| `[lmfao-page]` | https://lutter.cc/unshackled/assembler.html |
| `[lmfao-tgz]` | https://lutter.cc/unshackled/lmfao-0.1.5.tar.gz (LMFAO v0.1.5: `README`, `CHANGELOG`, `src/*`) |
| `[bf-hell]` | https://lutter.cc/unshackled/brainfuck.hell (the "complex example" for LMFAO) |
| `[elvm-hell]` | https://github.com/shinh/elvm/blob/master/target/hell.c |
| `[elvm-readme]` | https://github.com/shinh/elvm/blob/master/README.md (section "HeLL") |
| `[eso-mbp]` | https://esolangs.org/wiki/Malbolge_programming |
| `[eso-mu]` | https://esolangs.org/wiki/Malbolge_Unshackled |
| `[eso-hell]` | https://esolangs.org/wiki/HeLL — **returns HTTP 404**. There is no HeLL article on esolangs.org as of 2026-10-02. HeLL/LMAO are only mentioned in `[eso-mbp]`. |
| `[spec]` | `docs/spec.md` of this repository |

Method:
- I read the documents above. The README/CHANGELOG files are the only prose docs. `[lmao-tgz: README]` says "there is no full specification of the HeLL programming language", and `[lmfao-page]` says "There is no documentation of the HeLL dialect". Where the docs are silent, I read the GPL sources to learn the behaviour, and say so (tag `src/...`).
- **[verified]** marks claims I checked by experiment. I built LMAO 0.5.6b and LMFAO 0.1.5 locally (gcc + bison + flex). I assembled the shipped examples and ran the LMAO output on a small classic Malbolge interpreter written to the reference semantics (`[spec]` §8 classic mode). Scratch files lived in the session scratchpad and are not part of the repo.
- **INFERENCE** marks my own conclusions that no source states.

---

## 1. Program structure

**Sections.**
- A HeLL file consists of `.CODE` and `.DATA` sections. Every expression must belong to one, so the file must begin with one of the two keywords `[lmfao-tgz: README]`.
- Sections may alternate any number of times. The grammar is a sequence of Code or Data sections `[lmao-tgz: src/lmao.y]`.
- The examples freely interleave the two. `example_hello_world.hell` starts with a `.DATA` block (the string), then `.CODE`, then `.DATA` again `[lmao-tgz: example_hello_world.hell]`.

**Comments** `[lmao-tgz: README]`:
- Line comments start with `;`, `%`, `#` or `//`.
- Block comments are `/* … */`.

**Blocks** (the unit of placement):
- Blocks are separated by a *blank line*, which must not even contain a comment. Alternatively a block is enclosed in `{` `}` `[lmao-tgz: README]`.
- Inside braces, blank lines no longer split, "so you may use braces to connect your code if you want to use blank lines for clarity" `[lmfao-tgz: README]`.
- In the lexer every `{` and `}` also acts as a block separator. Nested braces are an error, and `.CODE`/`.DATA` inside braces is an error `[lmao-tgz: src/lmao.l]`.
- The assembler may rearrange blocks freely. Cells *within* a block stay contiguous ("commands that must not be seperated must be in the same code block") `[lmfao-tgz: README]`.
- Use blank lines "as often as possible" because this lets the assembler pack memory better `[lmfao-tgz: README]`.

**Labels** `[lmao-tgz: README]` `[lmao-tgz: src/lmao.l]` `[lmao-tgz: src/lmao.y]`:
- A label is an identifier followed by `:`. Identifiers match `[A-Za-z_][0-9A-Za-z_]{0,99}`.
- Each block must start with a label, or with an offset directive followed by a label.
- A label may not be a command name (`Nop`, `MovD`, …), and may not start with `R_` or `U_`.
- Duplicate labels are an error. Labels share one global namespace across code and data.
- A label must be followed by a word in its block ("Label definition must be followed by a data or code word"). In `.CODE`, a label may appear anywhere except at the end of a block `[lmfao-tgz: README]`.
- Several labels may name the same cell. The hello-world example puts two labels on one data word.

**Offsets.**
- `.OFFSET n` or its short form `@n` may appear only at the start of a block, before its first label. It pins the block to an absolute address `[lmao-tgz: README]`.
- In LMAO the operand is a single constant token: decimal, `0t…`, `C0`…`EOF` or a character. Expressions are not allowed `[lmao-tgz: src/lmao.y, rule Offset]`.
- Lutter's advice is to use it only "if really necessary". The examples use it once each, to pin a branch sled at `C21` (see §6) `[lmao-tgz: example_*.hell]`.

**Token separation.** Adjacent words must be separated by whitespace, otherwise the lexer reports "Misformed identifier". Operators, `,` and `@` reset that requirement `[lmao-tgz: src/lmao.l]`.

**Mandatory label.** `.DATA` must define `ENTRY` `[lmao-tgz: README]` (§3.4).

---

## 2. `.CODE` semantics

### 2.1 What a code word is

Each code word describes one memory cell as an **xlat2 cycle description** `[lmao-tgz: README]`. The commands are `Nop`, `Jmp`, `MovD`, `Opr`, `Rot`, `In`, `Out` and `Hlt`.
- **A single command** (no slash) is "not loop resistant". It is guaranteed only for its *first* execution. The assembler picks any character that decodes to that command at the address and ignores what it becomes after encryption `[lmao-tgz: README]` `[lmao-tgz: src/main.c, is_xlatcycle_existent]`.
  - Use it for code that runs once, for `Hlt`, and for `Jmp`. A `Jmp` is never itself encrypted, because the encryption hits the *new* `C` `[eso-mbp]` `[spec §5 notes]`.
- **A cycle** is written `A/B/C/…`, for example `MovD/Nop` or `In/Nop/Out/Nop/Nop/Nop/Nop/Nop/Nop`. It lists the command at the cell for the 1st, 2nd, … execution, wrapping around. The cell starts in the first state `[tut]`.
  - `Nop/MovD` is a Nop that becomes a MovD after one encryption `[tut]`.
  - **Matching rule (from source):** a `Nop` in a cycle matches any decode that is not one of the seven real instructions, so invalid characters count as Nop. The assembler rotates leading `Nop`s away and starts from the first real command. It then follows xlat2 and requires the character to return to its start after exactly *len* steps `[lmao-tgz: src/main.c, is_xlatcycle_existent]`.
  - So the written length must be a multiple of the real xlat2 cycle length of that character. The xlat2 cycle lengths are 2, 4, 5, 6, 9 and 68 `[spec §3.1]`.
- If the requested cycle exists at no position modulo 94, LMAO fails with "Forced xlat cycle doesn't exist" `[lmao-tgz: src/main.c]`.
- **Cycles used in the shipped examples:**
  - 2-cycles: `MovD/Nop`, `Nop/MovD`, `Opr/Nop`, `Rot/Nop`, `In/Nop`, `Out/Nop`.
  - 4- and 5-cycles: `Nop/Nop/Nop/MovD` and `Nop/Nop/Nop/Nop/MovD`.
  - 9-cycles: `MovD/Nop×8`, and the "evil" `Opr/Nop/Nop/Nop/Nop/Rot/Nop/Nop/Nop`.
  - Sources: `[lmao-tgz: example_hello_world.hell, example_cat_halt_on_eof.hell]`; the 9-cycle `In/…/Out/…` is in `[tut]`.
  - A `Rot/Nop/Nop/Nop` cycle does not appear in any source I read. Whether it is legal depends only on whether xlat2 has such a cycle at some position (the rule above).
- **Only 2-cycle pair.** The only 2-cycle in xlat2 is characters 70 ↔ 74 (`F` ↔ `J`). `MovD/Nop` exists only at positions ≡ 60 or 64 (mod 94). At both positions only the MovD character is a *loadable* initial value, so `Nop/MovD` can never be preloaded; it must be written by init code `[tut]`.
- **`RNop`** is short for `Nop/Nop`, a "loop-resistant Nop": an **immutable Nop** that behaves as Nop forever `[lmao-tgz: README]` `[lmfao-tgz: README]`.
  - An immutable Nop exists for every address `[eso-mbp]`.
  - LMAO takes one immutable-Nop character per address mod 94 from a hard-coded 94-entry table. The source comment credits Lou Scheffer and notes a better choice might be loadable `[lmao-tgz: src/main.c]`. Most of these characters are not loadable, which forces runtime initialization of the block.
  - A literal `Nop/Nop` takes the same code path as `RNop` (all-Nop cycle) `[lmao-tgz: src/main.c]`.

### 2.2 Code unit and the instruction cycle

**INFERENCE (consistent with every example and the tutorial):** a "code unit" is a code block of the form *label, one or more cycles, `Jmp`*. The last `Jmp` returns control to the data stream (§3).
- Typical units are `X: Op/Nop` + `Jmp` for each of MovD, Opr, Rot, In and Out. A bare `NOP: Jmp` is used to "immediately start execution of the command specified by the next .DATA memory cell" `[lmao-tgz: example_cat_halt_on_eof.hell]`.
- `HALT: Hlt` has no Jmp.
- Units may be longer. Examples:
  - `MovD/Nop×8`, `RNop`, `RNop`, `Opr/…/Rot/…`, a second label, `MovD/Nop×8`, `Jmp` `[lmao-tgz: example_hello_world.hell]`.
  - ELVM's `MovD/Nop`, `RNop`×3, `MovD/Nop`, `Jmp` `[elvm-hell]`.

**Placement by LMAO** `[lmao-tgz: src/main.c, main / add_codeblock_to_memory_layout]`:
1. For each of the 94 start residues, check that the cycle of cell *i* of the block exists at residue `(start+i) mod 94`.
   - If, for some residue, every cell's starting character is also *loadable*, the block can be **preinitialized**: written directly in the program text.
   - Otherwise it must be written by init code.
2. The block goes first-fit at the lowest free address with an allowed residue. The **cell before the block is reserved** (`RESERVED_CODE`).
   - Reason: a jump to the block's label encrypts that cell (§2.4) `[tut]`.
   - LMAO gives that cell a valid character: 81, or the last opcode in 33..126 seen before, alternating `[lmao-tgz: src/initialize.c, generate_opcodes_from_memory_layout]`.
3. Fixed-offset blocks (`@n`) are placed exactly at `n`, and cell `n-1` is reserved. A wrong residue makes LMAO report an error `[lmao-tgz: src/main.c]`.

### 2.3 `RNop` in practice

`RNop` serves two purposes:
- **D-padding:** every executed instruction increments `D`, so RNops move `D` forward over data words `[eso-mbp]`.
- **Sleds** for data-dependent jumps (§6.4).

The `U_` prefix (§4) makes LMAO insert RNops automatically in front of a block `[lmfao-tgz: README]`.

### 2.4 Jmp and the encryption of the cell before the target

Malbolge executes, then encrypts `[C]`, then increments `C` and `D`. A `Jmp` sets `C := [D]`; the cell at that *new* `C` is encrypted; then `C` is incremented `[tut]` `[spec §5]`. Consequences:
- **Every reference is stored as address − 1.** Execution then resumes exactly at the label. The same holds for `MovD` (`D := [D]`, then `D+1`), so LMAO decrements *every* label reference, code or data, without exception `[tut]` `[lmao-tgz: src/initialize.c, parse_dataatom]`.
  - If the label is at address 0, the stored value wraps to `C2` (59048) in LMAO, or to a head-2 value in LMFAO `[lmfao-tgz: src/initialize.c]`.
- **The `Jmp` itself is not encrypted.** The cell *before* the target is encrypted on every jump `[tut]` `[eso-mbp]`.
  - That cell must always hold a value in 33..126, otherwise the reference interpreter may crash `[eso-mbp]`. This is why LMAO reserves it (§2.2).
  - The data section must not overlap it `[tut]`.
- **The restore trick.** Jumping to `X+1`, which is `R_X`, encrypts cell `X` and advances its cycle by one. Execution then continues at `X+1`, normally the unit's `Jmp`.
  - For a 2-cycle this flips the cell back. It "restores" a used `Op/Nop` cell, or toggles a flag `[tut]`.
  - Jumping into the *middle* of a block encrypts the preceding cell. The hello-world example uses a label placed mid-block to "partially restore" a 9-cycle without executing its `Rot` `[lmao-tgz: example_hello_world.hell]`.

---

## 3. `.DATA` semantics

### 3.1 D-driven execution

The "program logic" lives in `.DATA` `[lmfao-tgz: README]`. **INFERENCE** (derived from the execution rule, and matching the tutorial's step-by-step trace `[tut]`):
- Suppose a unit's `Jmp` executes while `D = p`.
  - It sets `C := [p]` (target − 1), encrypts that cell, and then `C = target`, `D = p+1`.
  - The unit's instruction then sees `[D] = [p+1]` as its operand, and `D` becomes `p+2`.
  - The unit's closing `Jmp` reads `[p+2]` as the next target.
- So the data list is the program, and **`D` is the program counter**. Each "instruction" in `.DATA` is a run of words: a code reference, the operand words consumed by that unit, and then the next code reference.
- Typical lines:
  - `ROT C2 R_ROT`: target ROT; operand `C2` at `p+1`; next target `R_ROT` at `p+2`, which restores ROT and then runs that unit's Jmp on `[p+3]`.
  - `OUT ?- R_OUT`: Out ignores `[D]`, but `D` still advances, so a filler word is needed.
- **`MovD` is the goto.**
  - The word after a MovD unit's target is the new `D` (label − 1). After the increment, `D` = label, and the unit's `Jmp` executes `[label]`.
  - So `MOVED L` means "continue the data program at `L`".

### 3.2 What a data word can be

`[lmao-tgz: README]` `[lmfao-tgz: README]`:
- **A number**: decimal, `0t…` ternary, a named constant or a character (§5). It is stored as is, with **no** decrement.
- **A label reference**, optionally `R_`/`U_` prefixed (§4). It is stored as *address − 1* (§2.4). A code label used this way is a jump target; a data label is a `MovD` target or a pointer value.
- **An expression** over numbers and labels (§5.2).
- **A string**, `"…"`.
  - Escapes are `\r \n \t \\ \" \0`.
  - `"…", w` places word `w` between consecutive characters, not after the last one. The README's example expands `"Test", 42` to `'T' 42 'e' 42 's' 42 't'`.
- **`?`, don't care.** The cell is reserved, and its initial value is irrelevant `[lmao-tgz: README]`.
  - LMAO does not initialize it (from source). It holds whatever the memory fill gives: 81 or C1−81 (§7.3) `[lmao-tgz: src/initialize.c]`.
- **`?-`, unused.** The cell is never accessed. It is not reserved, so LMAO may place other small blocks there. Touching it is undefined behaviour.
  - It "Must not follow a label definition (in this case, use "?" instead)" `[lmao-tgz: README]`.
  - LMAO's `-d` debug mode treats `?-` as `?` `[lmao-tgz: CHANGELOG v0.5.1]` `[lmao-tgz: src/lmao.l]`.
  - The canonical use is the dead operand slot of `In`, `Out` and `RNop` paths.

### 3.3 Data is never preloaded

LMAO always puts data blocks into the "to be initialized" region. They are written by init code even when a value would be loadable. Fixed-offset data inside the preinitialized program area is rejected ("Invalid offset in data section") `[lmao-tgz: src/main.c, src/initialize.c]`.

### 3.4 How `ENTRY` starts execution

`[lmao-tgz: README]`: at start, `D` points at `ENTRY`, `C` points at *some* `Jmp`, and `A` is undefined.
- The tutorial adds that `A` currently happens to be `ENTRY − 1`, but programs must not rely on this. You must not read `A` before writing it with `Rot` or `In`. `Opr` does not count, because it reads `A` `[tut]`.
- The init code ends by moving `D` to `ENTRY` and executing a Jmp `[lmao-tgz: src/gen_init.c, generate_jump_to_entrypoint_with_module_system]` `[lmao-tgz: src/initialize.c]`.
- Therefore **the first word at `ENTRY` must be a code reference**: the first unit to run `[tut]`.

---

## 4. Label prefixes

There are exactly two prefixes, `R_` and `U_` `[lmao-tgz: src/lmao.l]`. Names such as `PARTIAL_R_LOAD_CHARACTER` are ordinary labels: the prefix is recognized only at the start of a reference, and labels may not start with `R_`/`U_`.

### 4.1 `R_` (restore)

- `R_X` means `X + 1`. After the automatic decrement, the stored word equals the plain address `X` `[lmao-tgz: README]` `[tut]`.
- Jumping through it encrypts cell `X` (restoring or toggling its cycle) and continues at `X+1` `[tut]`.
- It is only allowed for code labels whose block has a cell after `X`. Otherwise LMAO reports "Invalid use of R_ prefix" `[lmao-tgz: src/main.c, handle_U_and_R_prefix_for_data_atom]`.
- The README's intended use: "restore loop-resistant commands that are followed by a Jmp instruction directly" `[lmfao-tgz: README]`.
- Example (my own): a data line `CRAZY v R_CRAZY` runs `Opr` on `v`, which flips `CRAZY` to its Nop state. It then jumps to `CRAZY+1`, which flips it back, and that unit's Jmp continues with the next word.

### 4.2 `U_` (auto-RNop)

Syntax: `U_X Y`. **The two tokens make one data cell.** `Y` is not stored; it is an operand of the prefix (grammar rule `Dataatom: U_PREFIXED_IDENTIFIER IDENTIFIER`) `[lmao-tgz: src/lmao.y]`.
- `X` must be a code label at the start of its block ("prefix-free code").
- `Y` must be a data label later in the *same* data block `[lmfao-tgz: README]` `[lmao-tgz: src/main.c]`.

Semantics `[lmao-tgz: README]` `[lmao-tgz: src/main.c, handle_U_and_R_prefix_for_data_atom]`:
- Let *k* be the number of cells between the cell after the `U_` word and `Y`. The README's example: `U_ref foo 0 0 foo: 1` gives k = 2.
- The stored value is `X − 1 − k`, and LMAO ensures that the *k* cells before `X`'s block are immutable Nops. It reuses existing `RNop` cells there and prepends virtual `RNop`s if needed. It fails with "Cannot construct nop chain" if a non-Nop is in the way.
- Effect: the Jmp lands *k* cells before `X`, and the RNop sled advances `D` by *k*. `X`'s instruction then operates on `Y`, and the unit's `Jmp` reads the word after `Y`.

Example (my own): a variable reachable through several entry words.

```
op_v:   U_OPR v       ; k = 2 -> two RNops before OPR
rot_v:  U_ROT v       ; k = 1
v:      0
        ...           ; return dispatch (section 6.2)
```

A `MovD` to `op_v` makes `OPR` act on `v`. A `MovD` to `rot_v` makes `ROT` act on the same `v`. This is exactly the "variable with accessor entries" pattern of the examples and of ELVM `[lmao-tgz: example_hello_world.hell]` `[elvm-hell, emit_hell_variables_base]`.

`U_NOP Y` (where `NOP: Jmp`) is the pure form. It means "skip forward to `Y`" and is used as a dispatch entry (§6.4).

---

## 5. Constants, literals and expressions

### 5.1 LMAO (classic, 10 trits)

Sources: `[lmao-tgz: README]` `[lmao-tgz: src/lmao.l]`.
- **Decimal:** at most 5 digits (leading zeros allowed) and ≤ 59048, otherwise "Integer too big".
- **Ternary:** `0t` plus 1 to 10 trits.
- **Characters:** `'c'`, with escapes `\n \r \t \\ \'`. A string's `\0` exists only inside strings.
- **Named constants:**

| Name | Ternary | Decimal |
|---|---|---|
| `C0` | `0t0000000000` | 0 |
| `C1` | `0t1111111111` | 29524 |
| `C2` | `0t2222222222` | 59048 |
| `C20` | `0t2222222220` | 59046 |
| `C21` | `0t2222222221` | 59047 |
| `EOF` | = `C2` | 59048 |

### 5.2 Operators and evaluation

- The operators are `+ - * / >> << !`. The README's example is `(0t1210>>1)-1` `[lmao-tgz: README]`. Operators on label names were added in v0.4.2 `[lmao-tgz: CHANGELOG]`.
- **Precedence**, from the grammar, lowest first. All operators are left-associative.
  1. `>>` and `<<`
  2. `!`
  3. `+` and `-`
  4. `*` and `/`
  5. atoms and `( … )`
  - The right operand of `*` and `/` must be an atom or a parenthesized expression `[lmao-tgz: src/lmao.y]`.
- **Evaluation time:** at assemble time, *after* layout. A label leaf evaluates to `address − 1` (with `R_` already +1), and the operators then apply to those values `[lmao-tgz: src/initialize.c, parse_dataatom/parse_datacell]`.
  - So `L − 2` stores `addr(L) − 3`, which as a jump target lands at `L − 2`.
  - The difference `L1 − L2` is the true distance.
- **`a ! b`** = `crz(a as the A register, b as the memory operand)`, computed with the reference trit table `[lmao-tgz: src/malbolge.c]` `[spec §3.2]`.
  - Typical use: `C2!'H'` is the value that becomes `'H'` when crazied with `A = C2`. This works because `crz(C2, ·)` swaps the trits 1 and 2 and is an involution (§6.5) `[lmao-tgz: example_simple_hello_world.hell]`.
- **`a >> n` / `a << n`:** tritwise rotation of a 10-trit word. `<< n` is implemented as `10 − n` right rotations. `n ≥ 10` gives a warning and is reduced mod 10 `[lmao-tgz: src/initialize.c]`.
  - Typical use is a pre-rotated one-shot operand for `Rot`, for example `ROT 'e'<<1 R_ROT` `[lmao-tgz: example_simple_hello_world.hell]`.
- **Range quirk (from source):** a result above 59048 is reduced `% 59048`, not `% 59049`. Negative results are increased by 59049 `[lmao-tgz: src/initialize.c]`. This looks like a minor bug; our assembler should define wrap-around explicitly or forbid it.

### 5.3 LMFAO (Malbolge Unshackled, 3-adic)

**Number model.** Every number is a pair *(head trit, signed offset)* `[lmfao-tgz: src/typedefs.h]`. Literal forms `[lmfao-tgz: README]` `[lmfao-tgz: src/lmfao.l]`:
- `0t…`, `1t…`, `2t…`: the digit before `t` is the **head**, and the digits after it are the low trits, most significant first. `1t00212` = `...100212`.
- `C<h><trits>` and `...<h><trits>` mean the same thing. `C12` = `...12` = `1t2`; `...1221` = `1t221`.
  - The classic names fall out of this: `C0` = 0, `C1` = `...1`, `C2` = `...2` (= −1), `C21` = `...21` (= −2), `C20` = `...20` (= −3).
  - Classic 10-trit values must be written with an explicit head 0. For example `C02222222222` is the natural 59048 `[bf-hell]`.
- Decimal literals are unbounded (`long long`). The keyword `EOF` no longer exists `[lmfao-tgz: src/lmfao.l]`.

**Operators** `[lmfao-tgz: src/initialize.c, parse_datacell]` `[lmfao-tgz: src/malbolge.c]`:
- `+` and `-` are 3-adic, with head propagation.
- `*` and `/` reject head-1 operands ("operand in C1-region not defined").
- `!` is a head-aware 3-adic `crz`.
- **`>>`/`<<` become trit shifts** (÷3, ×3), because the rotation width is unknown at assemble time. LMFAO prints "Error: Rotation operator (>>) replaced by shift operator" when a non-head trit falls off. It is a message, not an abort.

**Head-2 data values cannot be constructed.** LMFAO aborts with "unconstructable value" `[lmfao-tgz: src/highlevel_initializer.c, change_0_1_trits]`.
- **[verified]** `example_simple_hello_world.hell` fails under LMFAO for this reason, after the shift warning.
- `brainfuck.hell` uses `C2` only inside expressions, never as a data word `[bf-hell]`.

---

## 6. Flags, branching and restore idioms

### 6.1 Unconditional goto and the landing-pad convention

- `MOVED L` (with `MOVED: MovD/Nop` + `Jmp`) continues at `L` and leaves MOVED in its Nop state. Every jump target therefore begins with `R_MOVED`, which re-arms it `[lmao-tgz: example_*.hell]`.
- If a label can be reached both by fall-through and by `MOVED`, the fall-through path first executes an extra `R_MOVED` to *disarm* the unit. The common `R_MOVED` at the label then re-arms it in both cases. The example comments this as "destroy MOVED, because it will be restored in the next step" `[lmao-tgz: example_cat_halt_on_eof.hell]`.

### 6.2 Flags = 2-cycle conditional MovD (subroutine return)

A flag is a unit `FLAGk: Nop/MovD` + `Jmp`.
- **Normal state:** Nop ("clear").
- **`R_FLAGk`:** jumping through it encrypts the cell, which *sets* the flag (MovD state), and execution continues.
- **The data pair `FLAGk target`:**
  - If the flag is set, the MovD executes and goes to `target`. The encryption returns the flag to Nop, so test-and-branch clears it automatically.
  - If the flag is clear, the Nop executes, the encryption *sets* it, and `D` skips over `target`. The idiom therefore always writes `FLAGk target R_FLAGk`; the trailing `R_FLAGk` clears the flag again on the not-taken path.

This gives calls with return addresses, from `[lmao-tgz: example_hello_world.hell]`, `[lmao-tgz: example_cat_halt_on_eof.hell]` and `[elvm-hell, emit_call/emit_function_footer]`:
- The caller writes `R_FLAGk` and then `MOVED sub`.
- The subroutine ends with a chain `FLAG1 ret1 R_FLAG1`, `FLAG2 ret2 R_FLAG2`, …, one entry per call site.
- The cost is O(number of call sites) per return.

### 6.3 Counters (cycle length > 2)

A `MovD` inside a *k*-cycle branches on every *k*-th pass, and falls through (skipping its target word) on the other passes.
- Examples:
  - `MovD/Nop` "test if we did it twice": branch on pass 1, fall through on pass 2.
  - `Nop/Nop/Nop/Nop/MovD` "exit loop after 5 times".
- The counter resets itself after a full cycle. This is how the examples get loops of 2, 4, 5 and 10 (= 5 × 2) iterations, for example 10 rotations of a 10-trit word `[lmao-tgz: example_hello_world.hell, example_cat_halt_on_eof.hell]`.
- Every branch and fall-through advances the cycle. The *k*-cycle state is therefore part of the program state; it is not "reset" between uses.

### 6.4 Data-dependent branches (computed jumps into a sled)

**LMAO `@C21` trick** `[lmao-tgz: example_hello_world.hell, example_cat_halt_on_eof.hell]`:
- A fixed block at `C21` holds `RNop` (at 59047), `RNop` (at 59048) and `Jmp` (at 0, after wrap-around).
- A variable is first driven by crazy operations to either `C20` or `C21`. Then `MOVED var` executes, and the Jmp uses the variable's runtime value as target.
- Entering at 59047 or at 59048 executes 2 or 1 RNops before the Jmp at 0. `D` therefore ends on different words, which are `U_NOP` dispatch words placed after the variable.
- The variable's normal accessor path (`U_CRAZY var` → Opr → next word) uses a third `U_NOP` word ("execution not jumped to carry").
- **INFERENCE:** cell 0 is also the startup `Jmp` of LMAO's fixed init module (`'b'`, which decodes to Jmp at address 0). LMAO forbids anything at cells 0 and 1 other than `bP` `[lmao-tgz: src/initialize.c]`, so the sled's final `Jmp` coincides with it.
  - **[verified]** The debug layout of `example_hello_world` has the CARRY block's `Jmp` at address 0.

**Unshackled analogues:**
- `brainfuck.hell` computes `1t0` or `1t1` and "goes to" it: `1t0` means continue, `1t1` means break `[bf-hell, rotwidth loop comment]`.
- ELVM gives every variable three `U_NOP` dispatch words and tests it with a MovD onto the variable (`emit_test_var`) `[elvm-hell]`.
- **INFERENCE:** these targets lie in LMFAO's reserved head-1 window around `C1` (§8). Presumably the init module provides the sled there. I did not trace the 19391-character module to confirm.

### 6.5 Restore and operand idioms

- **`ROT C2 R_ROT`, `ROT C1 R_ROT`, `ROT C0 R_ROT`:** the uniform constants are fixed points of rotation, so the operand cell survives and the line is reusable. `A` receives the constant `[lmao-tgz: example_*.hell]` `[spec §3.4]`.
- **`ROT x' R_ROT`** with a pre-rotated `x'` (for example `'e'<<1`) is one-shot, because the cell is rotated in place.
- **`CRAZY c R_CRAZY`:** Opr writes its result back into `c`. It is reusable only if `c` is a fixed point for the expected `A`.
  - With `A = C1`, `crz(C1, d)` maps the d-trits 0→0, 1→0, 2→2. A constant with only 0/2 trits is therefore unchanged and copied into A. This is why ELVM's immediates after `ROT C1` use only 0/2 trits in the first operand `[elvm-hell, emit_load_immediate]` `[spec §3.2]`.
- **`OUT ?- R_OUT` and `IN ?- R_IN`:** a dead operand slot, then the restore `[lmao-tgz: example_simple_cat.hell]`.
- **Non-destructive read of a cell into A:** load `A = C2` (or ELVM's all-2 value `1t22…2`) and crazy the cell. That swaps its 1 and 2 trits. Do it twice (with a 2-counter): the cell is restored, and A holds the original. Hello world comments this as "crazied C2 into a character … must do this twice to read the original" `[lmao-tgz: example_hello_world.hell]` `[elvm-hell, emit_read_0tvar]`.
- **Store A into a cell:**
  1. Set the target and a helper to `C1` by crazying `C1` into each twice.
  2. Crazy A into the helper. This swaps the 0/1 trits.
  3. Crazy the result into the target, which unswaps them.
  - Sources: `[eso-mbp, "Store operation"]` `[elvm-hell, emit_clear_var/emit_write_var]`.
- **Restoring long cycles:** a 9-cycle must be advanced 9 times. Hello world restores its 9-cycle "LOAD_CHARACTER" unit with repeated `R_` jumps, a 4-counter and a 2-counter. A mid-block label skips the dangerous `Rot` state `[lmao-tgz: example_hello_world.hell]`.

---

## 7. What LMAO generates around the user program

### 7.1 Output layout (classic memory 0..59048)

Sources: `[lmao-tgz: src/main.c, put_all_memcells_together]` `[lmao-tgz: src/initialize.c]`. **[verified]** with `-d` debug output.

| Region | Content |
|---|---|
| 0 … 1432 | A **fixed 1433-character init module**. It begins with `bP`: cell 0 is a Jmp that skips to address 99. It contains a **constant-generation data module** at about 33–96 `[lmao-tgz: datamodule.txt]`. **[verified]** length 1433. |
| … | **Generated init code**: straight-line normalized Malbolge over `o j p * i`, denormalized per position. It ends with a Jmp, with `D` at `ENTRY`. |
| … | **Filler**: random characters from the 8 normalized instructions (`rand()`, seeded by time), so **LMAO output is not deterministic** `[lmao-tgz: src/initialize.c]` `[lmao-tgz: src/main.c, srand(time(NULL))]`. |
| up to `L−3` | **Preinitialized code blocks** (those loadable at their residue), packed just before the end. |
| `L−2`, `L−1` | The two characters **`RQ`**, placed at a residue (16, 17, 35, 51, 52, 74, 80 or 93 mod 94) where both are valid instructions. LMAO requires this position to be ≥ 500. |
| `L` … 59048 | Never-written fill. On top of it lie the **runtime-initialized code blocks and all data blocks**, packed against the top of memory with residues mod 94 preserved. |

**Why `RQ`?** "Program should end with "RQ" ⇒ crazied cells will have good values for us" `[lmao-tgz: src/main.c]`. The fill after the program then alternates **81 and C1−81** (`0t0000010000` and `0t1111101111`). The init code relies on these known starting values `[lmao-tgz: src/gen_init.h, State.last_preinitialized]`.
- **[verified]** on three outputs: fill = 29443, 81, 29443, ….

**Measured placements [verified]:**
- `example_simple_cat`: code units at 1999, 2017 and 2034 (preinitialized, near the end of the 2050-character program); data at 59032 and above.
- `example_hello_world`: data at ~58650–58760; CARRY at 59047, 59048 and 0.

### 7.2 Init code: how values are written

Source: `[lmao-tgz: src/gen_init.c, generate_normalized_init_code_for_word_with_module_system]`. Cells are initialized in ascending address order. For each cell:
1. The old value is known: 81 or C1−81 by parity.
2. A `DESTINATION` pointer cell in the data module is set to the target address − 1.
   - If the next target is within 9 cells, the pointer is advanced "by 9".
   - Otherwise a new address is crazied into it.
3. The A value needed to turn the old value into the target by one Opr is solved trit by trit (`get_register_a_value_for_crazy`).
   - It is impossible when the cell has a 0 trit and the target needs a 2 there, or the cell has a 2 and the target needs a 0 (rows of the crz table). In that case the cell is first reset to `C1` by crazying `C0` into it. Neither starting value contains a 2, so this always gives `C1`, from which every target is reachable.
4. A is loaded from the constant module (rotations and crazies with `C0`, `C1`, `C2` and `C21` cells), choosing a cheap combination.
5. `MovD` to `DESTINATION`, Nops to step `D` onto the target, then `Opr`.

Finally the code moves `D` to `ENTRY` and emits the closing `i` (Jmp).

### 7.3 The "only 8 loadable characters" restriction

At every address only one character per instruction (8 in total) is loadable `[tut]`. LMAO handles this in four ways:
- **Code:** a block is written into the program text only if all its starting characters are loadable at its chosen residue. Otherwise the init code writes it (§2.2).
- **Data:** always written by init code (§3.3).
- **Init code:** emitted as normalized code, so it is loadable by construction `[lmao-tgz: src/gen_init.h, denormalize_malbolge]`.
- **Unknown fill:** avoided by the `RQ` ending (§7.1).

### 7.4 Search and options

- LMAO first guesses an init-code size (2/3 of a first attempt) and retries with +32 cells until layout and init code fit without overlap `[lmao-tgz: src/main.c]`.
- `-f` ("fast mode", bigger output) skips the search and falls back automatically on failure (`CHANGELOG` v0.5.3).
- `-l n` inserts line breaks.
- `-d` writes a `.dbg` file with labels, source positions, steps until `ENTRY` and xlat2 positions `[lmao-tgz: src/main.c]`.

### 7.5 Size overhead

**[verified]** with LMAO 0.5.6b. Cells = HeLL cells placed, including reserved and pad cells, counted from the debug file.

| Example | HeLL cells | Output chars | Steps before `ENTRY` | Run |
|---|---|---|---|---|
| `example_simple_cat` | 15 | 2050 | 1859 | echoes, never halts; after EOF prints byte 168 forever |
| `example_simple_hello_world` | — | 4607 | — | prints `Hello, World!\n`, halts after 4474 steps |
| `example_hello_world` | 206 | 14740–14795 | 14427 | prints `Hello, World!\n`, halts after 27937 steps |
| `example_cat_halt_on_eof` | 287 | 20775 | 19928 | echoes `abc`, halts on EOF |
| `example_digital_root` | — | 31754 | — | not run |
| `example_adder` | — | 55724 | — | not run |

- **INFERENCE:** cost ≈ 1433 fixed characters plus **~65–70 characters of init code per runtime-initialized cell**, with init time ≈ program length in steps.
- Programs are capped at 59049 characters. The CHANGELOG mentions effort to fit "large Malbolge programs near the size limit" (v0.5.6b).
- The sizes vary slightly between runs and modes (14740 with `-d`, 14795 without).

---

## 8. LMFAO (Malbolge Unshackled)

### 8.1 Unshackled background

`[eso-mu]`:
- Values are 3-adic: a head with repeating trits.
- **The rotation width is variable.** It starts at ≥ 10, and grows when a `j` (MovD) makes `D` wider than half the rotation width. It does not change as long as `D` stays within its previous maximum width.
- It is unspecified whether auto-increment counts.
- Fill beyond the program continues the period-6 pattern by remainder mod 6.

### 8.2 What LMFAO changes

- **Notation:** 3-adic literals (§5.3). LMFAO is based on LMAO v0.5.4 (`[lmfao-tgz: CHANGELOG]` v0.1). It has no `-f` and no debug output: the debug writer is commented out `[lmfao-tgz: src/main.c]`.
- **Layout:** blocks with fixed offsets are placed first, then the others `[lmfao-tgz: src/main.c, main]`.
  - Unfixed code and data are appended sequentially in the **head-1 region**, starting at `C1+41` (`...1 + 41`).
  - Code blocks additionally need a matching residue mod 94 and a reserved predecessor cell.
  - Head-1 addresses from `C1−41` to `C1+37` are reserved: "canot initialize cell in reserved memory area" `[lmfao-tgz: src/highlevel_initializer.c]`.
  - Fixed offsets at head-0 addresses ≤ 1 or head-2 addresses fail with "Code cannot be initialized at given offset".
  - **[verified]** Because of this, LMAO's `@C21` examples (hello world, halting cat) do **not** assemble with LMFAO; `C21` is now the head-2 value `...21`.
- **Output:** the fixed **19391-character init module**, then generated init code. **[verified]** The module string in the source is exactly 19391 characters, and the code notes `c = 19391`.
  - The code is padded with `o` until the position is ≡ 17 (mod 94), and two normalized `<` are appended `[lmfao-tgz: src/highlevel_initializer.c, generate_malbolge]`.
  - **INFERENCE:** this tail plays the role of LMAO's `RQ`. Under 3-adic semantics a suitable two-character ending makes the fill alternate `0t10000` (81) and `1t01111`. **[verified]** by computing the 3-adic crz sequence from an `RQ` ending.
  - ELVM and `brainfuck.hell` rely on exactly these two values: "uninitialized cells, which are initially 0t10000", and every memory pair is followed by `1t01111`, a back-jump pointer to `@1t01112` `[elvm-hell]` `[bf-hell]`.
- **Rotation-width handling: `init_all`** `[lmfao-tgz: src/highlevel_initializer.c]`:
  - It starts with `min_rot_width = 10`.
  - While that is below the width the program needs, it doubles the width. It builds a `...0222…2` constant, crazies it to get a `...100…0` address, does a MovD there, and jumps back via a computed "back-jump address" (German comments: "erzeuge … ...0222222 …, crazie das in C0, springe ans ziel mit movd; kehre von dort zurück").
  - This matches `[eso-mu]`: a MovD to a wide address *forces* a lower bound on the width. The width can be forced up but never measured.
- **Per-cell init:**
  1. For each cell, `initialize_memory_cell` computes the width needed by the address and the value.
  2. If the current width is too small, the needed width is raised and the **whole init sequence restarts** from scratch.
  3. Otherwise it builds the value and the address into scratch variables (`set_gen_init_03`) and calls a rotation routine inside the fixed module.
  - All values first have their 0 and 1 trits swapped (`change_0_1_trits`). This is why head-2 values are unconstructable (§5.3).
- **Start:** LMFAO writes a `Jmp` character into the head-1 cell `...120` and jumps to `ENTRY` from there (source comment) `[lmfao-tgz: src/highlevel_initializer.c, init_all]`.
- **Size:**
  - **[verified]** `example_simple_cat` → 28219 characters.
  - Lutter's `brainfuck.hell` (3698 lines) is the reference example `[lmfao-page]` `[bf-hell]`. The size of its `.mu` output was not re-measured here.
- **Programs must cope with the unknown width at run time.**
  - `brainfuck.hell` has an "increase_rotwidth" routine: a MovD to `1t011…1` "will double the rotwidth".
  - It has a "loop over rotwidth" that uses a rotating `1t2` value as a counter, and it generates `1t22…2` filling the whole width `[bf-hell]`.
  - ELVM does the same (§9).

---

## 9. The ELVM HeLL backend (`target/hell.c`)

Target: HeLL for **LMFAO**, that is Malbolge Unshackled `[elvm-readme]`. The README says it is not tested by default because "Malbolge Unshackled is extremely slow". I/O is reduced mod 256, and newline conversion "cannot be revert[ed] in a platform independent way" `[elvm-readme]`. Lowering model `[elvm-hell]`:

- **Registers → HeLL variables.**
  - ELVM registers `A B C D BP SP`, plus `ALU_SRC`, `ALU_DST`, `TMP`, `CARRY`, `VAL_1222`, `TMP2` and `TMP3`, are each one data block (`emit_hell_variables_base`). The block contains:
    - two entry words `opr_X` and `rot_X`. Each first toggles the *other* unit with `R_ROT` or `R_OPR` and then enters via `U_OPR X` or `U_ROT X`, so one common exit `R_ROT R_OPR` restores both;
    - the value cell, initialized to 0;
    - three `U_NOP` dispatch words for a zero/one test;
    - a return chain of `VAR_RETURNk` flags.
  - Values are 24-bit naturals (head 0).
  - **INFERENCE:** the symmetric entry/exit trick is a reusable pattern for our register file.
- **Instruction → sequence of calls.**
  - Each ELVM instruction is lowered to: copy operands into `ALU_DST`/`ALU_SRC` (clear to C1, non-destructive read, store), call an ALU routine, copy the result back (`hell_emit_inst`).
  - Calls use `R_<FLAGGROUP>k` + `MOVD func`. Each function ends with a chain of return-flag tests (`emit_call`, `emit_function_footer`).
- **Immediates:** loaded with `ROT C1`, then one to three `OPR` constants (`emit_load_immediate`). The constants are chosen so the operand cells are not destroyed (§6.5).
- **Arithmetic:**
  - `add`/`sub` work trit-serially inside a **rotation-width loop**. Each iteration does increment/decrement with carry on the lowest trit, then rotates `ALU_DST`, `ALU_SRC` and the helper `1t22…2`.
  - The compute part runs for at most 20 iterations ("execute the following code at most 20 times"), and the rotate part runs for the full width, so values return to their positions (`emit_rotwidth_loop_*`).
  - `add_uint24`/`sub_uint24` add 2^24 and reduce with `modulo`, which is repeated subtraction.
  - Comparisons are built on `sub` plus a test.
  - `VAL_1222` (= `1t22…2` over the full width) is generated once at `ENTRY` (`emit_generate_val_1222_base`).
  - A dummy data word `UINT_MAX*9` exists "to force rot_width to be large enough" during LMFAO init (`init_state_hell`).
- **Memory:**
  - ELVM memory is a HeLL data array in the head-1 region. It is pinned with `@1t…` offsets, starting at `1t1111111111111022222` and stepping down by 2. Each word is a value cell followed by `1t01111`.
  - Values are stored with an offset of +81 (`0t10000`), so never-written cells read as 0 (`init_state_hell`, `emit_read/write_memory_base`).
  - **Address computation:** `memptr = (MEMORY_0 − 2) − 2·addr` (`emit_compute_memptr_base`).
  - **Access:** a 9-cycle unit `MovD … RNop RNop Opr/…/Rot/… MovD …` moves `D` to `memptr` and Oprs the cell. The cell's companion word `1t01111` then MovDs `D` back to the fixed label at `1t01112` ("backjump"). After that, the 9-cycle unit is restored with the same loop trick as LMAO hello world (`emit_memory_access_base`).
- **Control flow:**
  - Every ELVM pc gets a label `label_pcN`; direct jumps are `MOVD direct_jmp_label_pcN`.
  - Conditional jumps: compare, test the result into a `CARRY_IS_C1` flag, then `FLAG target` (`hell_emit_inst`, `JEQ`…).
  - Indirect jumps use a lookup table pinned at `@1t200000`: one `MOVD label_pcN` pair per pc. The comment says this is to keep the pointer "computable within rotwidth of 20".
  - The backend computes `table + 2·pc`, converts the `0t` result to a `1t` address, and jumps via a `MovD`-`RNop×3`-`MovD`-`Jmp` unit (`emit_jmp_alusrc_base`, `emit_branch_lookup_table`).
- **I/O:**
  - `putc` takes mod 256 and then runs `OUT ?- R_OUT`.
  - `getc` runs `IN ?- R_IN`. It recognizes Unshackled's special newline/EOF encodings by size and last trit, maps them to `\n` and 0, and reduces everything else mod 256 (`emit_getc_base`).

**Takeaways for our HLA → asm lowering (INFERENCE):**
1. Use a fixed register file of data blocks with accessor entries.
2. Use a tiny ALU of flag-returning subroutines.
3. Indirect memory needs runtime-computed `MovD` targets plus a back-jump word.
4. Indirect jumps need a dispatch table.
5. Use one RNop sled per branch-on-value.

With a known rotation width (our language), the rotation-width loops can be unrolled or bounded exactly, and `VAL_1222`-style constants can be assembled directly.

---

## 10. Example walk-throughs (own words)

**`example_simple_cat.hell`** (57 lines with license header) `[lmao-tgz]`:
- `.CODE` has three 2-cycle units: MovD/Nop, In/Nop and Out/Nop, each followed by `Jmp`.
- `.DATA` is one braced block. Its first word is a loop label that restores the MovD unit. `ENTRY` follows directly. The program then reads into A (dead operand slot), restores In, writes A (dead slot), restores Out, and finally MovDs back to the loop label.
- So one pass = In, Out, goto.
- There is no EOF test, so after EOF it prints byte 168 (= `2222222222t mod 256`) forever. **[verified]**, matching `[eso-mbp]`.
- About 15 cells → 2050 characters.

**`[tut]` cat** (the tutorial's variant):
- It uses a single 9-cycle `In/Nop/Out/Nop×6` cell. One pass reads, a later pass writes, and the passes in between are Nops.
- It shows the hand assembly: choosing residues from Scheffer's cycle tables (In cycle at 37, MovD/Nop at 60), loading the loadable data words directly, initializing the rest with use-once Opr/Rot code, and ending with a MovD and a Jmp to `ENTRY`.
- **INFERENCE:** a useful model for a minimal, non-LMAO startup sequence.

**`example_simple_hello_world.hell`**:
- Three reusable units (Opr, Rot, Out), plus one non-loop-resistant block of seven `Rot`, `Out` pairs and a `Hlt`.
- The data list prints each character in one of two ways:
  - load A with `ROT C2`, then crazy a precomputed `C2!'c'` word;
  - `ROT` a pre-rotated character (`'c'<<1`).
- Either way, it then outputs.
- The last seven characters are printed by the straight-line block. Its operand words (pre-rotated characters interleaved with `?-`) follow the jump word directly in data.
- Every character cell is destroyed by use, so this is one-shot code. **[verified]** prints `Hello, World!\n` (4607 characters).

**`example_hello_world.hell`** (loop-based, "techniques … from reverse engineering of the 99 bottles" program):
- **Data:** the string is stored as character, `data_read`, character, `data_read`, …, with `last_data_read` after the last character.
- **Code:**
  - three 2-cycle flags, plus two named flags (`NO_MORE_CARRY_FLAG`, `NO_MORE_DATA_FLAG`);
  - 2-, 4- and 5-counters;
  - Opr, Rot, Out and MovD units, a `NOP` (bare Jmp) and a `HALT`;
  - a 9-cycle `LOAD_CHARACTER` unit, which MovDs through the string pointer, Oprs the character and MovDs onward;
  - the `@C21` CARRY sled.
- **Variables:** `string_ptr` (with Opr, load and Rot entry words via `U_`), `carry` (with the sled dispatch) and `tmp`, each with a flag-return chain.
- **Main loop:**
  1. Load the current character into A non-destructively (crazy with C2 twice via a 2-counter).
  2. The character's following word (`data_read` or `last_data_read`) decides whether the end flag is set.
  3. Out.
  4. If the end flag is set, halt.
  5. Otherwise increment `string_ptr` by 2 with a hand-built ternary increment. Per trit: crazy a `C2` into `ptr`, `tmp` and `carry` twice. `carry` ends as `C20` or `C21`, the sled branches on it, and the routine rotates and repeats, giving 10 rotations (5 × 2) per increment.
  6. Restore the 9-cycle unit (9 restores, without executing its Rot state).
- **[verified]** 206 cells → about 14.8k characters. It prints `Hello, World!\n` after 14427 init steps, and 27937 steps in total.

**`example_cat_halt_on_eof.hell`**:
- **Code:** 11 flags, counters 2/2/5, the usual units, and the `C21` sled.
- **Data:** four variables `tmp1`–`tmp4`. Each has a crazy entry; `tmp2` also has a Rot entry; `tmp4` also has the three `U_NOP` sled words, leading to `HALT` or to "no EOF".
- **One pass:**
  1. Set all four variables to `C1`, each crazied with C1 twice through a 2-counter.
  2. `In`.
  3. Store A by crazying into `tmp1`, `tmp2`, `tmp3` and `tmp4` in sequence. `tmp2` and `tmp4` then hold the character.
  4. Test for EOF (C2) trit-wise. Ten times, rotate `tmp2` and combine it into `tmp4` with a fixed C1/C2/C1 crazy sequence. The program's own comment says this yields 2 only where both trits are 2. The result in `tmp4` is C2 or C0.
  5. Map that to `C21`/`C20` with three more crazies.
  6. MovD onto `tmp4` and jump into the sled.
  7. On EOF, halt. Otherwise read `tmp2` non-destructively (crazy C2 twice), `Out`, and loop.
- **[verified]** 287 cells → 20775 characters. It echoes its input and halts at EOF.

---

## 11. Open questions and pitfalls for an LMAO-like assembler on malbolge-unleashed

Our machine `[spec §2.2–§2.4, §3.4, §5]`:
- Values are 3-adic.
- The rotation width is `n`. It starts at `n = max(10, width(L−1)+1)`, grows only by the growth rule `n := max(n, width(C)+1, width(D)+1)`, never shrinks, and is capped at 39.
- There is no wrap-around.
- Never-written cells hold `P[x mod 6]`.
- Encrypting a non-instruction is `E_ENCRYPT`.

Pitfalls (INFERENCE unless cited):

1. **Rotation width is deterministic, so `>>`/`<<` can be real rotations, but only for a fixed `n`.**
   - LMAO hard-codes 10; LMFAO degrades them to shifts (§5).
   - Our assembler should take `n` as a layout parameter. It should evaluate `<<`/`>>` as head-preserving rotations of the low `n` trits `[spec §3.4]`, and **guarantee that `n` does not change while the program runs**. Otherwise pre-rotated operands (`'e'<<1`) and every "loop over the width" break.
   - Two rules: every runtime-initialized address (and every value ever loaded into `C`/`D`) has width ≤ `n−1`; and the startup code raises `n` to its final value, if at all, *before* the first `Rot`.
   - Open question: should the assembler *force* `n`? (For example, a startup MovD to a width-`(n−1)` address, which is the deterministic analogue of LMFAO's `init_all`.) Or should it only accept `n = max(10, width(L−1)+1)`?
2. **No wrap-around at 59048.**
   - LMAO's `@C21` sled (RNop at 59047/59048, Jmp at 0) and its "label at 0 → `C2`" decrement both rely on the classic wrap.
   - In our language `0t2222222222` has width 10, and its increment is `0t10000000000`. Using it as an address raises `n`.
   - The 3-adic equivalents work: `...21` (−2) and `...2` (−1) have width ≤ 1, and `...2 + 1 = 0` `[spec §1.3]`. So sleds and branch values should use the head-2 constants `...20 / ...21 / ...2` (LMFAO's `C20 / C21 / C2`), not the classic 10-trit numbers.
   - Head-2 addresses decode like classic addresses near 59048, because `C mod 94` uses `R_2 = 59048`; head-1 addresses decode like addresses near 29524 `[spec §1.4]`.
3. **Classic 10-trit constants are different values here.**
   - `C1` must be `...1`, not `0t1111111111`.
   - `C2`/`EOF` must be `...2`. Our `In` returns `...222` at EOF `[spec §6]`, so LMAO's 10-rotation EOF test (halting cat) is both unnecessary and wrong.
   - The "store via C1 / read via C2 twice" identities hold trit-wise including the head, but only with the head-extended constants.
   - Recommendation: adopt LMFAO's literal syntax (`0t/1t/2t`, `C<h>…`, `...<h>…`) and drop the 10-trit `C20 / C21 / EOF` meanings. Open question: should `EOF` be kept as an alias for `...2`?
4. **Fill values and `E_ENCRYPT`.**
   - Our fill after an `RQ`-style ending alternates `0t10000` (81) and `1t01111`. **[verified]** by computing `crz` 3-adically from `R`, `Q`.
   - That gives LMAO's known starting values and LMFAO/ELVM's free back-jump pointer.
   - But `1t01111` is head 1: encrypting it, or executing it, is `E_ENCRYPT`/`E_EXEC`. Every cell *before* a jump target (the reserved cell, §2.4) must be explicitly written with a valid character, as LMAO does `[lmao-tgz: src/initialize.c]`. Any never-written cell that can be reached by `C` is fatal.
   - Open question: which residues mod 94 allow a loadable ending pair that gives a period-2 fill? LMAO's list (16, 17, 35, 51, 52, 74, 80, 93) applies to head-0 program cells and should carry over unchanged, because decoding of cells `0..L−1` is classic. Re-verify this with our VM.
5. **Init-code cost and the layout region.**
   - LMAO packs runtime data near 59048. That has width 10, which needs `n = 11`, while `n = 10` for `L ≤ 19683`.
   - Our layout should instead put runtime-initialized blocks in `[L, 3^(n−1))` or in the head-1/head-2 windows (3^(n−1) cells each), as LMFAO does with head 1. The 3-adic arithmetic of addresses then needs §5.3-style (head, offset) numbers in the assembler.
6. **Width of computed pointers.**
   - `Rot` results generally have width `n` `[spec §3.4]`, so a rotated value must never be used as an address. ELVM-style trit-serial arithmetic must rotate a full `n` times to restore values before they are used as pointers.
   - Computed `MovD`/`Jmp` targets (memory arrays, jump tables) must be built with width ≤ `n−1`. This is what ELVM does by pinning its table at `@1t200000` "within rotwidth of 20" `[elvm-hell]`.
   - The assembler should statically bound such ranges and warn.
7. **We must write our own startup and init modules.**
   - LMAO's 1433-character module and LMFAO's 19391-character module are GPLv3 binaries-as-strings. They must not be copied.
   - LMFAO's module mostly exists to cope with an *unknown* width, which we do not have.
   - The tutorial's hand-assembly shows the minimal ingredients `[tut]`: separate `C` and `D` first (MovD/Jmp at 0), save the uniform constants, build values with Opr from loadable neighbours, then MovD and Jmp to `ENTRY`.
   - LMAO's per-cell algorithm (§7.2) is a reasonable model to re-implement from the description.
8. **Determinism.**
   - LMAO's filler uses `rand()` seeded by time, and its size search is heuristic.
   - Our assembler should be deterministic: a fixed filler (for example normalized `o`, or a stated pattern) and reproducible layout.
9. **Cycle semantics are unchanged; residues are the same for program cells.**
   - xlat2 and `(C + [C]) mod 94` are classic for head-0 cells `[spec §3.1, §3.5]`. All cycle-existence tables carry over for cells inside the program text.
   - For runtime-written cells at head-1/head-2 addresses, the residue is `C mod 94` under the remainder rule. The assembler must use that rule when it searches residues (as LMFAO does with `Number` addresses).
10. **Grammar details that are under-specified in HeLL** and should be decided in our spec:
    - expression grammar and precedence (§5.2; LMAO's shifts bind loosest, which is unusual);
    - whether offsets may be expressions;
    - character escape sets (LMAO's char lexer accepts `\'` but its switch never assigns a value for it: a probable bug);
    - `?-` immediately after a label (forbidden);
    - labels pointing to `?-` (LMAO only warns);
    - `U_` requiring the target at the start of a block, with only RNops before it;
    - whether `R_` applies only to code labels;
    - integer range and wrap of expression results;
    - whether `A` at `ENTRY` is defined (LMAO: no).
11. **Docs are silent on:**
    - a formal grammar (none exists; the README is "a short overview");
    - LMFAO's dialect ("There is no documentation of the HeLL dialect" `[lmfao-page]`);
    - the internals of both init modules;
    - the exact semantics of LMFAO's reserved head-1 window and its startup `Jmp` at `...120`.
    - Everything above about those internals comes from reading source comments, not documentation.
