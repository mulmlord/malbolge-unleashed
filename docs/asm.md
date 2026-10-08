# malbolge-unleashed assembly (`.mua`)

Status: design, issue #12. Normative for the assembler tickets #13–#17.
Sources:
- the language: `docs/spec.md`
- background on HeLL/LMAO/LMFAO: `docs/research/hell-research.md`, cited below as `[H §x]`
- the placement tables: `src/asm/cycles.hpp`

The assembly language is modelled on HeLL, the language of Matthias Lutter's LMAO/LMFAO assemblers. HeLL has no formal specification `[H §11]`, so this document is the specification for **our** dialect. It keeps HeLL's model and most of its surface syntax. It changes everything that depended on the classic 10-trit machine or on Malbolge Unshackled's unknown rotation width. The changes from HeLL are listed in §11.

## 1. Execution model

A program is a set of **code blocks** and **data blocks**. The assembler places them in memory and generates startup code that writes everything that can't be part of the loaded program text. Data is never preloaded (spec §4).

- **D is the program counter.** The `.DATA` section holds the program logic as a list of words, and `.CODE` holds small units, typically one instruction followed by `Jmp` `[H §3.1]`. Suppose a unit's `Jmp` executes with `D = p`:
  1. `C := [p]` (the target − 1). The cell at the new `C` is encrypted, then `C` = target and `D = p+1`.
  2. The unit's instruction runs with `[D] = [p+1]` as its operand. `D` becomes `p+2`.
  3. The unit's closing `Jmp` reads `[p+2]`, the next target.
- **A reference to label `X` is stored as `X − 1`** (§5), so that execution resumes exactly at `X` after the Jmp's increment `[H §2.4]`. The same holds for `MovD` targets.
- **`Jmp` is never encrypted.** The cell *before* the jump target is encrypted (spec §5 step 4). Every code block therefore has a **reserved predecessor cell**. The assembler always writes a character in 33..126 into it, because a never-written cell there would fault with `E_ENCRYPT` (spec §2.4, §7) `[H §11.4]`.

## 2. Lexical structure

- **Encoding:** ASCII. Identifiers, keywords and command names are case-sensitive.
- **Comments:** `;`, `#` or `//` to end of line, and `/* … */` (not nested).
- **Identifiers:** `[A-Za-z_][A-Za-z0-9_]*`, at most 100 characters. An identifier must not:
  - equal a command name (§3.1) or a named constant (§4.2);
  - start with `R_` or `U_` (those are reference prefixes, §5.2).
- **Labels:** `name:` defines a label. There is one global namespace for code and data, and duplicates are an error.
- **Blocks:** a block is a run of non-blank lines. A **blank** line is empty or whitespace-only; a line holding only a comment is *not* blank. A blank line ends a block. So do `.CODE`, `.DATA`, `{` and `}`.
  - Alternatively, `{ … }` encloses a block. Blank lines inside braces, or inside a `/* … */` comment, do not split it. Braces don't nest. `[H §1]`
  - `{` and `}` may share a line with other tokens, e.g. `.DATA {` or `ENTRY: … }`.
- **Words per line:** a line may hold several words, code words included. Line breaks have no meaning inside a block except as whitespace.
- **Labels in detail:**
  - A label may precede any word, including mid-block, and several labels may name one cell.
  - A label must be followed by a word in its block.
  - Reserved words: the command names, `RNop` and the named constants (§4.2).
- **Sections:** `.CODE` and `.DATA` switch sections. They may alternate any number of times. Before the first section, a file may contain only comments, blank lines, an optional `.WIDTH` (§8.1) and `.INCLUDE` lines (§8.3); `.WIDTH` must come before any `.INCLUDE`. A block outside any section is an error. `.CODE`, `.DATA` and `.INCLUDE` inside braces are errors.
- **Directives** (§8): `.WIDTH`, and `@` / `.OFFSET` at the start of a block.
- **Token separation:** adjacent words need whitespace between them. Operators, `,`, `(`, `)`, `@`, `/` (between cycle commands) and `:` (after a label) separate on their own, so spaces around them are optional. `//` always starts a comment. Tokens are read greedily, e.g. `?-1` is `?-` directly followed by `1`, which is an error (missing whitespace).
- **A blank line ends an expression**, also inside braces, where it does not end the block: a binary operator (or a string's `,` separator) after a blank line does not continue the previous word.
- **Lines in data blocks.** The line a data word starts on is part of the program's meaning for one rule: §6 treats the *first word of a line* as a code-reference position. The printer (`malm asm --print`) therefore keeps the source's line grouping of data words.

Every block starts with a label, optionally preceded by an offset directive (§8.2); a block whose first cell has no label is an error. An empty block (`{ }`, or an offset with no cells) is an error. The assembler may place blocks anywhere, but the cells *within* a block stay contiguous and in order.

## 3. Code blocks

### 3.1 Code words

A code word describes one cell. Its commands are `Nop Jmp MovD Opr Rot In Out Hlt` (spec §3.5).

- **A single command**, e.g. `Opr`, is guaranteed only for the cell's **first** execution. The assembler picks any character that decodes to that command at the cell's position and ignores what it becomes after encryption. Use it for one-shot code, for `Hlt` and for `Jmp`.
- **A cycle**, e.g. `MovD/Nop`, lists the command executed on the 1st, 2nd, … execution, wrapping around. The cell starts in the first state.
  - **Matching rule:** `Nop` in a cycle matches *any* decode that is not one of the seven real instructions. The written length must be a multiple of the cell character's xlat2 cycle length, which is 2, 4, 5, 6, 9 or 68 (spec §3.1). The rule follows `[H §2.1]`.
- **`RNop`** is a Nop forever. `RNop`, and any cycle written entirely of `Nop`s (`Nop/Nop`, …), matches every character whose **whole** xlat2 cycle decodes to Nop at the position, regardless of cycle length. This is an exception to the length rule above.
  - The assembler picks the loadable `o` if it is immutable there, otherwise the smallest such character (`rnop_char` in `cycles.hpp`).
  - Such a character exists at every position, but the loadable `o` is immutable at only 14 of them, so most RNops are written by the startup code.
  - `RNop` is a reserved word.
- If a requested cycle exists at **no** position mod 94, that is an assembly error.

Where a cycle exists is a property of the position mod 94 (`op_sequence` in `cycles.hpp`). For example, the only 2-cycle (F↔J) gives `X/Nop` only for Rot at 59/63, Out at 25/29, In at 43/47, MovD at 60/64 and Opr at 82/86. Counters (§3.3) match against the full `op_sequence`. Placement (§7) respects these constraints.

- **`R_X` targets** must be a cycle cell that is not all-`Nop` (§5.2). On a single-command cell the state after encryption is unspecified, and on an `RNop` or all-`Nop` cycle restoring has no effect, so `R_X` there is an error.
- **`Jmp` in a cycle** is allowed, but executing a `Jmp` never encrypts its own cell (spec §5), so such a cell advances only through `R_` or other jumps that land just after it.

### 3.2 Code units and idioms

A **unit** is a label, one or more code words, then `Jmp`:

```
ROT:    Rot/Nop
        Jmp
```

- After its first run, a `X/Nop` unit is in its Nop state. Jumping to `R_X` (§5.2) encrypts cell `X` again, which flips it back. This is the **restore** idiom `[H §6.5]`.
- `HALT: Hlt` needs no `Jmp`.
- `NOP: Jmp` (a bare Jmp) means "continue with the next data word".

### 3.3 Flags and counters

- **Flag:** a unit `F: Nop/MovD` + `Jmp`, normally in the Nop state ("clear").
  - Jumping to `R_F` *sets* it.
  - The data triple `F target R_F` does this:
    - if the flag is set: branch to `target` (the flag clears itself);
    - if it is clear: fall through.
  - The trailing `R_F` re-clears the flag on the fall-through path `[H §6.2]`.
  - Subroutine return uses one flag per call site.
- **Counter:** a `MovD` inside a k-cycle, e.g. `Nop/Nop/Nop/Nop/MovD`, branches on every k-th pass `[H §6.3]`. The available lengths are those of xlat2: 2, 4, 5, 6, 9.
  - A counter's state is program state. Every pass advances it, whether it branches or falls through.

## 4. Numbers and constants

### 4.1 Values

All values are 3-adic, exactly as in spec §1. The assembler computes with the language's own value model (`core::Value`), so a value has a head and a width ≤ 39.

### 4.2 Literals

| Form | Meaning |
|---|---|
| `123`, `-1` | Decimal. Negative values have head 2 (`-1` = `...2`, spec §1.3). |
| `0t2101` | Head 0, the trits most significant first (spec §1.1). At least one trit after `t`. |
| `1t00212`, `2t1` | Head = the digit before `t`, then the low trits (at least one). `1t00212` = `...100212`. |
| `...1102` | Spec §1.1 notation: the first digit is the head; `...1` alone is the head with no low trits. |
| `'c'` | A character code. Escapes: `\n \r \t \0 \\ \' \"`, `\xHH` (00..FF). Raw control or non-ASCII bytes inside a literal are errors; use an escape. |
| `"text"` | Data only: one word per byte, same escapes; `""` is an error. `"ab", w` puts the word `w` (an expression, `?` or `?-`) *between* consecutive bytes `[H §3.2]`. A string in `.CODE` is an error, as is a command name in `.DATA`. |

**Named constants** are 3-adic, not the classic 10-trit numbers:

| Name | Value | Notes |
|---|---|---|
| `C0` | `0` | |
| `C1` | `...1` | `crz(a, C1)` swaps a's trits 0↔1 |
| `C2` | `...2` = −1 | fixed point of rotation |
| `C21` | `...21` = −2 | |
| `C20` | `...20` = −3 | |
| `EOF` | `...2` | what `In` returns at end of input (spec §6) |

`C2`, `C21`, `C20` and `EOF` have head 2: they are fine inside expressions, but not as the value of a data word (§4.3, §6).

### 4.3 Expressions

Expressions are evaluated at assembly time, **after layout**.

**Operators**, from highest to lowest precedence. All binary operators are left-associative:

1. unary `-` (3-adic negation) and `( … )`
2. `!`: `a ! b` = `crz(a, b)`, where `a` plays the A role and `b` the memory operand (spec §3.2). Example: `C2 ! 'H'` is the operand that turns into `'H'` when crazied with `A = C2`.
3. `<<` and `>>`: rotate the low `n` trits left or right (spec §3.4). `n` is the program width (§8.1).
   - `x >> k` is `rotr` applied k times, and `x << k` = `x >> (n − k)`.
   - `k` must be a constant natural with 0 ≤ k ≤ n.
   - The rotated operand `x` must have width ≤ `n`.
4. `+` and `-`: 3-adic addition and subtraction.

**Extent across whitespace.** Data words are separated by whitespace, so an expression continues across whitespace while the next token is a binary operator (longest match).
- `ROT -1 R_ROT` is two words: `ROT − 1` and `R_ROT`. Write `ROT (-1) R_ROT` for three words.
- `?`, `?-`, strings and `U_X Y` are whole words and cannot appear inside expressions.

**Label leaves:** a plain label `X` evaluates to `addr(X) − 1`, and `R_X` to `addr(X)` (§5).
- So `L − 2` stores a value that, used as a jump target, lands at `L − 2`.
- `L1 − L2` is a true distance when both labels lie in the same window (§7) `[H §5.2]`.

**Results:** results are exact 3-adic values. There is no silent wrap-around (cf. LMAO's `% 59048` quirk `[H §5.2]`).
- A data word whose value has width > `n` is an error, because the spec §2.2 invariant forbids such cells.
- A data word whose value has head 2 is an error: the startup code cannot build it (§6).
- The stored value of a label may have width `n`: e.g. the cell before the lowest address of a head-1 window, see §6.

## 5. Data blocks

### 5.1 Data words

| Word | Stored value |
|---|---|
| number or expression | its value, unchanged |
| `X` (label) | address(X) − 1 |
| `R_X` | address(X), i.e. `X + 1` after the decrement (§5.2) |
| `U_X Y` | one cell; see §5.2 |
| `"…"` | one cell per byte |
| `?` | a reserved cell; its initial value is unspecified |
| `?-` | an unused cell, never accessed. Other blocks may overlap it. No label may name it. |

### 5.2 Reference prefixes

**`R_X`, restore:** the stored value is `addr(X)`, so execution continues at `X + 1`.
- Jumping through it encrypts cell `X`, flipping or advancing its cycle, and continues at `X + 1`.
- `X` must be a code label naming a **cycle** cell that is not all-`Nop` (§3.1), with at least one more cell after it in its block `[H §4.1]`.

**`U_X Y`, auto-RNop:** occupies **one** cell. `Y` is not stored; it is the prefix's operand.
- `X` must be a label of the **first cell** of a code block (any of its labels), and `Y` a data label of a later cell in the same data block.
- Let `k = addr(Y) − addr(U word) − 1`, the number of cells strictly between them. The stored value is `addr(X) − 1 − k`.
- The assembler makes sure the `k` cells before `X` are immutable Nops, placing RNops there if needed. If another cell occupies one of them, that is an error.
- When several `U_` words name the same `X`, the RNop run has length `k_max`, and the block's reserved cell (§7) moves to `addr(X) − 1 − k_max`.
- Effect: the jump lands `k` cells early, the RNop sled advances `D` by `k`, and `X`'s instruction operates on `Y`. This is the "variable with accessor entries" pattern `[H §4.2]`.

### 5.3 `ENTRY`

`.DATA` must define `ENTRY`, and its first word must be a **code reference**: a code label, `R_X` with `X` a code label, or a `U_` word. Expressions (e.g. `X − 1`) are not accepted there. A missing `ENTRY` is reported at line 1, column 1 of the root file. When execution reaches `ENTRY`, the startup code guarantees:
- `D = address(ENTRY)`, and `C` is at a `Jmp`;
- `n` equals the program width (§8.1);
- every cell of every block holds its declared initial value, and every reserved predecessor cell holds a character in 33..126;
- the startup code has performed no I/O;
- **`A` is unspecified.** Write `A` with `Rot` or `In` before reading it.

## 6. Branching on values

There are no conditional instructions. A value-dependent branch jumps *through the value itself* into an RNop sled, so that `D` ends on different words `[H §6.4]`.

**Head 2 is out of reach.** `crz` maps heads 0 and 1 to heads 0 and 1 (spec §3.2: `T[0][0] = T[1][0] = 1`, `T[0][1] = T[1][1] = 0`), rotation keeps the head, the loader and the fill produce only heads 0 and 1, and an increment never turns head 0 or 1 into head 2. So no cell holds a head-2 value unless `In` returned EOF (`...2`), and `C` and `D`, which are loaded from cells, never reach a head-2 address. The assembler therefore rejects head-2 data words (§4.3) and head-2 `@` offsets (§7); head-2 constants such as `C2` remain useful inside expressions (`C2 ! 'H'`).

**Cell 0.** Cell 0 of every assembled program is the startup `Jmp` (`b`), and cells 0..98 belong to the startup code (§7). Cell 0 stays a `Jmp` only if the program never encrypts or overwrites it. The assembler enforces the static part, and the program is responsible for the rest:
- **Rejected by the assembler:**
  - any block, reserved cell, `@` offset or `U_` RNop run at addresses 0..98;
  - a constant data word with value `0` in a code-reference position, i.e. the first word of a data line (§2, "Lines in data blocks"), or the operand of a code unit known to `Jmp`/`MovD` through it.
- **Forbidden at run time** (not checkable): jumping through the value 0, which encrypts cell 0, and `Opr`/`Rot` with `D = 0`.

**Sleds.** A sled is an ordinary code block of `RNop`s ending in a `Jmp`:

```
SLED:   RNop
        RNop
        Jmp
```

How a branch runs:
1. The program drives a variable `var` to one of the values `SLED`, `SLED+1` or `SLED+2` (a label leaf is the address − 1, §4.3), typically with `crz` against a table of such words.
2. `MOVED var` moves `D` onto `var`, and MOVED's `Jmp` then jumps through `[var]`. The value lands at `SLED`, `SLED+1` or `SLED+2`, so the sled executes 2, 1 or 0 RNops. The cell before the landing address is encrypted: the reserved cell or an RNop, both safe.
3. The sled's `Jmp` then reads `[var+3]`, `[var+2]` or `[var+1]`, typically `U_NOP …` dispatch entries `[H §6.4]`.

The one head-2 value a program sees, EOF, lands at cell 0 when jumped through (`...2 + 1 = 0`), and the startup `Jmp` there then continues with the next data word. That is why cell 0 stays a `Jmp`.

**Pointer safety (spec §2.2, "Consequences for code generators"):**
- Every address that `C` or `D` *visits* must have width ≤ `n − 1`. That includes the landing address, i.e. the stored value + 1.
- The stored value itself may have width `n`.
- Never use a `Rot` result as a pointer.
- The assembler checks constant data words in code-reference positions. Runtime values are the program's responsibility.

## 7. Layout and generated code

This section is the contract for #14 and #15. `src/asm/startup.hpp` describes the generated code in detail.

- **Determinism:** assembling the same source with the same options gives the same bytes. There is no randomness and no time-dependent filler (unlike LMAO `[H §7.1]`).
  - Placement is first-fit in source order. Ties between residues go to the lowest address.
  - The character for a reserved cell is chosen by a fixed rule (see "Characters" below).
- **The program text** is the startup code and nothing else: cell 0 is `b` (a `Jmp` to 99, with `D = 1`), cells 1..98 hold the startup code's constants and pointers, the startup code follows from cell 99, and the text ends with two characters chosen to make the fill predictable (spec §2.4). Its length `L` is the **base**: no user block, reserved cell or RNop run may lie below it (§6).
- **Windows.** User blocks live in two windows:
  - **head 0:** `[base, 3^(n−1) − 1)`;
  - **head 1:** all addresses of width ≤ `n − 1` with head 1, except the last.

  The last address of each window is left out because writing a cell leaves `D` on the next one, which would then have width `n`. Head-2 addresses are unreachable (§6), and a block pinned there is an error. Each block **and its reserved cell** lie wholly inside one window.
- **Runtime-written:** every block, reserved cell and RNop run is written by the startup code; none is part of the program text. (Preinitializing loadable code is a possible size optimization.) A block pinned below the base is an error.
- **Positions:** a code block's start position mod 94 is chosen so that every word's cycle exists there (§3.1).
  - For head-1 addresses, the position is `addr mod 94` under the remainder rule (spec §1.4).
  - Blocks with `@` are placed exactly; a residue that doesn't fit is an error.
- **Reserved cells:**
  - The cell before every code block (or before its `U_` RNop run, §5.2) is reserved and receives a character in 33..126.
  - No cell of another block may occupy it, except an RNop or a `?-` slot. In particular, two code blocks may not sit back to back, because B's reserved cell would be A's closing `Jmp`.
- **Sharing cells.** A block's **footprint** is its cells, plus, for a code block, its reserved cell and its `U_` RNop run. Footprint cells are either:
  - **soft**: an RNop or all-`Nop` cycle, a reserved cell, or a run cell. Each of these only needs a character fixed by its position, so soft cells of different blocks may share an address. A reserved cell that shares an address with an RNop holds the RNop.
  - **hard**: every other code or data cell. A hard cell needs its address to itself.

  A `?-` slot claims nothing, so a cell of any other block may sit on it.
- **First fit.** Pinned blocks are placed first, exactly where they are pinned. The other blocks follow in source order. Each one goes to the lowest footprint start in the head-0 window where its footprint fits and every code cell's cycle exists at its position. Only pinned blocks use the head-1 window.
- **Characters.** All of these are deterministic:
  - a single command: the loadable character for it at the cell's position;
  - a cycle: the smallest loadable character whose xlat2 cycle matches it there (§3.1), or the smallest matching character if none is loadable;
  - an RNop, an all-`Nop` cycle or a run cell: `rnop_char` (§3.1);
  - a reserved cell: the loadable `o` (Nop) at its position. It is encrypted when a jump lands after it, but it is never executed.
- **The image.** Layout gives every cell with a defined initial value: code cells, reserved cells, run cells and data words. A `?` cell has no defined value, so the startup code doesn't write it.
- **Writing the image.** The startup code writes the image cells in address order. For each cell `X` it builds two values in its work area, a pointer to a base `B ≤ X` a few cells below `X` (reused for the following cells) and an operand `a` with `crz(a, [X]) = V`. A `MovD` through the pointer and `X − B` Nops bring `D` to `X`, and one `Opr` writes `V`. `D` is then on `X + 1`, a fill cell, and a `MovD` there returns to the work area. If `[X]` has a trit 0 where `V` has a trit 2, a first `Opr` with `A = 0` turns `[X]` into `...111`. Values are built trit by trit with a rotating mask, at a cost of about 2 steps per position the mask passes and 5 per trit it changes. The costs are tracked by `tests/asm/startup_test.cpp`.
- **Width forcing.** Before building anything that depends on `n` (rotations), the startup code raises `n` from the initial `n` of the text to the program width `N`, one step at a time: a `MovD` through `swap01(rotr(f))`, built from a fill cell `f` whose trit 0 differs from its head, lands at width exactly `n` and so grows `n` by one. It is the deterministic analogue of LMFAO's `init_all` `[H §8.2]`. After forcing, `n` must not change while the program runs (§6, pointer safety).
- **No I/O:** the startup code performs no input or output.
- **Outputs** of `malm asm`:
  - a `.malm` (default), or text with `--text`;
  - optional `--map out.map`, used by `malm disasm` (#17). One line per label: `name`, its canonical address (spec §1.1 notation), `code` or `data`, and the extent (first and last cell) of its block.

## 8. Directives

### 8.1 `.WIDTH n`
- Sets the program width `n` (10 ≤ n ≤ 39). It is used for `<<`/`>>` and for width checks. The startup code forces it.
- **Default** (a deterministic search):
  1. Start with the smallest `n ≥ 10` at which the blocks could fit above the base, starting with base 100.
  2. Lay out the program at that base and `n`, then generate the startup code.
  3. If the text ends above the base, raise the base past the end of the text and repeat. If some address would need width ≥ `n`, or the text's length raises the initial `n` above `n` (spec §2.2), set `n += 1` and repeat.
  4. `n > 39` is an error.
- An explicit `.WIDTH` that is too small for the layout is an error.
- Programs that use `<<`/`>>` get the same result either way, because their values are computed for the final `n`. An explicit `.WIDTH` just makes it visible in the source.
- It may appear once, in the root file, before any section and any `.INCLUDE`.

### 8.2 `@expr` / `.OFFSET expr`
- Only at the start of a block, before its first label. It pins the block's first cell to the address `expr`. For a braced block it may stand directly before the `{` (`@1t0 {`) or right after it; `@expr {` also starts a new block when it directly follows an unbraced block's last line. An offset anywhere else in a block is an error.
- `expr` is a constant expression: no labels and no `<<`/`>>`, since the latter depend on `n`.
- The cell before a pinned code block is reserved like any other.

### 8.3 `.INCLUDE "path"`
- Textually includes another `.mua` file, so that shared units (e.g. a library of `Op/Nop` + `Jmp` units) can be reused.
- **Where:** only between blocks, on a line of its own. It acts like a blank line, so it ends the current unbraced block. Inside braces it's an error.
- **Path:** a string literal, resolved relative to the directory of the including file.
- **Section:** the included file starts in no section, so it must switch to a section before its first block (§2 applies to it like to the root file). After the include, the including file stays in the section it was in before.
- **Restrictions:** an included file may not contain `.WIDTH`. Including a file that is already being included (a cycle) is an error, while including the same file twice in sequence is allowed. Its labels share the global namespace.
- **Files:** paths are resolved like the operating system does (symlinks followed, then `..`), and two paths are the same file if they resolve to the same file; that is what cycle detection compares. Only regular files can be read (the root file too); devices, FIFOs and directories are errors. Diagnostics show the path as written.
- **Diagnostics** inside an included file name that file, followed by a `file:line:col: note: included from here` line (pointing at the `.INCLUDE`) for each level.
- **Limits:** at most 64 nested include levels and 1000 includes per program (an include "bomb" would otherwise grow exponentially), and at most 64 MiB of source text and 4 Mi cells in total over all files. Exceeding a limit is one error, after which the assembly stops.

## 9. Errors

Every diagnostic has the form `file:line:col: error: message`, followed where they help by `file:line:col: note: …` lines (e.g. the include chain, or the first definition of a duplicate label). An assembly reports as many independent errors as it can, not just the first, in source order. A syntax error suppresses the static checks (fix syntax first), and recovery skips to the end of the line without crossing `{`, `}`, a directive or a block boundary, so one mistake does not cascade.

Implementation limits: identifiers 100 characters (for `R_`/`U_` references, after the prefix); parenthesis nesting 128 and expression height 256; the source and include limits of §8.3; at most 1000 reported errors (the first 1000 in source order).

They cover:
- lexical and syntax errors;
- unknown or duplicate labels;
- misuse of `R_`/`U_`;
- `?-` after a label;
- cycles that don't exist;
- values wider than `n`;
- impossible layouts;
- head-2 data words and head-2 `@` offsets;
- a missing `ENTRY`.

**Two kinds of check:**
- **Static checks** need only the source and run before layout (#13):
  - syntax;
  - label rules and name resolution;
  - `R_` on a code label naming a cycle cell with a following cell in its block;
  - `U_` operand rules except its distance;
  - `?-` rules;
  - `ENTRY` present, with a code reference as its first word;
  - constant `@` expressions;
  - cycle existence at *some* position;
  - `.INCLUDE`.
- **Layout checks** run during placement and evaluation (#14/#15): positions, widths, windows, `U_` RNop runs, the cell-0 and pointer-safety rules.

## 10. Examples

These two programs are the reference examples. They become golden tests in #16, assembled and run in unleashed mode.

### 10.1 Hello world

Each character costs one `Rot` of a pre-rotated operand and one `Out`. The operand cell is rotated in place, so this is one-shot code.

```
.CODE
ROT:    Rot/Nop
        Jmp

OUT:    Out/Nop
        Jmp

HALT:   Hlt

.DATA
{
ENTRY:  ROT 'H'<<1 R_ROT    ; A := rotr('H'<<1) = 'H'; restore ROT
        OUT ?- R_OUT        ; write A; Out ignores its operand
        ROT 'e'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'l'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'l'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'o'<<1 R_ROT
        OUT ?- R_OUT
        ROT ','<<1 R_ROT
        OUT ?- R_OUT
        ROT ' '<<1 R_ROT
        OUT ?- R_OUT
        ROT 'w'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'o'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'r'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'l'<<1 R_ROT
        OUT ?- R_OUT
        ROT 'd'<<1 R_ROT
        OUT ?- R_OUT
        ROT '!'<<1 R_ROT
        OUT ?- R_OUT
        ROT '\n'<<1 R_ROT
        OUT ?- R_OUT
        HALT
}
```

Trace of the first line, with `p` = the address of `ENTRY`:
1. The startup `Jmp` reads `[p]` (the word `ROT`, i.e. ROT − 1), encrypts the reserved cell, and executes `Rot` at `ROT` with `D = p+1`. `A` becomes `'H'`, and `ROT` flips to Nop.
2. `ROT`'s `Jmp` reads `R_ROT` and encrypts `ROT` back to `Rot`.
3. `ROT`'s `Jmp` runs again and reads `OUT`.

### 10.2 Cat (endless)

Reads a byte and writes it, forever. After end of input, `In` gives `...2` (EOF), and `Out` writes `...2 mod 256` = 168, as in classic Malbolge (spec §6). A cat that halts on EOF needs a value-dependent branch (§6), and is the golden test in #16.

```
.CODE
IN:     In/Nop
        Jmp

OUT:    Out/Nop
        Jmp

MOVED:  MovD/Nop
        Jmp

.DATA
{
ENTRY:  R_MOVED             ; disarm MOVED: the R_MOVED at LOOP re-arms it
LOOP:   R_MOVED             ; landing pad: re-arm MOVED after the goto
        IN ?- R_IN          ; A := next byte
        OUT ?- R_OUT        ; write A
        MOVED LOOP          ; goto LOOP (D := LOOP − 1, then D = LOOP)
}
```

The goto works like this:
1. `MOVED LOOP` executes `MovD` with `[D] = LOOP − 1`, so `D` becomes `LOOP − 1`, and the encryption puts `MOVED` into its Nop state. The increment gives `D = LOOP`.
2. `MOVED`'s `Jmp` reads `[LOOP] = R_MOVED`, which re-arms `MOVED` and continues with `IN` `[H §6.1]`.

## 11. Differences from HeLL (LMAO/LMFAO)

| Topic | HeLL | Here |
|---|---|---|
| Value model | LMAO: 10-trit; LMFAO: 3-adic | 3-adic, spec §1 |
| `C1`, `C2`, `C20`, `C21`, `EOF` | LMAO: 10-trit numbers | 3-adic (`...1`, `...2`, …); `EOF` = `...2` |
| `<<` / `>>` | LMAO: 10-trit rotate; LMFAO: shift (unknown width) | rotation over the program width `n` |
| Rotation width | LMFAO: unknown, forced up by doubling | deterministic: `.WIDTH`, forced once at startup, fixed afterwards |
| Sled for value branches | `@C21` relying on the classic wrap 59048 → 0 | an ordinary RNop/`Jmp` block; head-2 values and addresses are unreachable (§6) |
| Precedence | `<<`/`>>` bind loosest, then `!`, then `+`/`-`, then `*`/`/` | `!` > `<<`/`>>` > `+`/`-`; `*` and `/` removed |
| Comments | `;` `%` `#` `//` `/* */` | `%` dropped |
| Expression range | `% 59048` wrap | exact; a word wider than `n` is an error |
| Output | `rand()` filler, time-seeded | deterministic |
| Reserved predecessor cell | holds a char in 33..126 (LMAO's fill alternates 81 and 29443; 29443 would break the reference interpreter too) | **must** hold a char in 33..126: otherwise `E_ENCRYPT` |
| Low addresses | LMAO forbids anything at 0..1 besides `bP` | the whole program text belongs to the startup code; cell 0 stays a `Jmp` for EOF branches (§6) |
| Escapes | `\'` broken in LMAO's char lexer | `\n \r \t \0 \\ \' \"`, `\xHH` |
| Startup module | 1433 / 19391 fixed GPL characters | our own, generated (#15) |

## 12. Clean-room plan

LMAO and LMFAO are GPLv3. The assembler is written from this document, `docs/spec.md` and `docs/research/hell-research.md`, which describe the behaviour in our own words.

- **Do not copy:**
  - no code, tables or strings from LMAO/LMFAO;
  - in particular, not their startup modules;
  - not their 94-entry RNop table: ours is computed from xlat2 in `cycles.hpp`.
- **Do not port:** the HeLL example programs are not ported verbatim. The examples here (§10) and the golden tests in #16 are written independently. The idioms they use (restore via `R_`, `OP ?- R_OP`, goto plus landing pad) are standard HeLL technique.
- **Compatibility is not a goal:** HeLL programs written for LMAO/LMFAO generally need edits. The 3-adic constants and the absence of wrap-around (§11) change their meaning.
- **Re-implement from the description:** the per-cell init technique (old value known from the fill, solve `A` trit by trit, crazy once, reset through `C1` when needed `[H §7.2]`) is a published algorithm description. It is re-implemented from that description, not from LMAO's source.
