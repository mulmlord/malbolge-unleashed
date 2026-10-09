# THROWAWAY: 32-bit word prototypes (spike #18)

**Throwaway code.** Nothing here is built, tested in CI or meant to be reused. It backs the
measurements in [ADR 0001](../../docs/adr/0001-hla-word-representation.md). The HLA (#19-#22)
reimplements what it needs.

## Files

| File | What it does |
|---|---|
| `tern.py` | A model of malbolge-unleashed values (40 trits, trit 39 = head), `crz`, `rotr`, and `load_seq`, which loads any constant into `A` with a `Rot` and up to four inline fixed-point `Opr`s |
| `search.py` | Cheapest-first search for trit-wise operations (`xor`, `and`, `or`, `mov`, column `sum120`, `reset1`) as sequences of `SET` (load a constant into `A`) and `OPR cell` |
| `mask_search.py` | The same, for the add-table index of one byte lane (`byte`: in a byte cell, `word`: in a cell with other lanes) |
| `gen.py` | Generates a `.mua` benchmark for `L` byte lanes per cell (`1` = four byte cells, `2` = two 16-bit halves, `4` = one word cell) and checks every sequence by simulation |
| `measure.py` | Runs `gen.py`, assembles with `malm asm`, runs with `--trace --stats`, counts steps between `Out` markers and checks the results read back after each operation |

```sh
cmake --preset release && cmake --build --preset release
python3 measure.py 1      # byte cells; also 2 (halves) and 4 (word cell)
python3 search.py xor     # one sequence; no argument: all of them
python3 mask_search.py word
```

`measure.py` writes into `out/` (ignored): the program, its map and `.malm`, and `result_L*.txt`.
A run takes 5 to 15 seconds and about 1.3 GB of memory for `L = 4` (see the ADR on page size).

## Machinery and its measured cost

All numbers are steps of the VM, measured with `--trace` on `malm run`.

- **Inline op** (`ROT C1 R_ROT`, `OPR k R_OPR` with the operand in the data stream): 3 steps.
- **Register op** (`R_F MOVED X_O` → `X_O: OPR X: value` → return chain `F RET R_F` → `RET: R_MOVED R_OPR`):
  9 steps, plus 3 for every flag of the register's return chain that is tested before the
  caller's own flag. The search uses 9.
- **SET** a constant into `A`: 3 steps per inline op, 1 to 5 ops (`tern.load_cost`): `C0`/`C1` take
  one `Rot`; a constant with head 1 at most a `Rot` and three `Opr`s, with head 0 four.
- **Dispatch** (`MovD` through a register into a table): `R_F MOVED I_M` → `U_MD I` → table entry
  `MD2 stream` → stream. The stream returns through flags like a register op.
- **RAM access**: `R_F MOVED P_A` → `U_RA P` → `RA: MovD/Nop/Nop/Nop RNop Opr/Nop/Nop/Nop Jmp`
  (2-cycle `MovD`/`Opr` pairs need 17 RNops between them, so 4-cycles are used) → record
  `[value][MB][BACK]` → `BACK: R_MB R_RA ?- ?- R_RA ?- ?- R_RA ?- ?-` (advance both 4-cycles three
  times) → return chain.

`measure.py` reports each operation twice: as measured, and without the 3 steps of every return
flag that was tested and skipped. The second number is the cost of the routine with an ideal
return; the first depends on how many call sites each register has in this particular program.

## Sequences

Bits are stored as the trits 0 and 2 ("f02"): with `A = C1`, `crz` is the identity on such a cell,
so a word is read into `A` by one `Opr`, and `crz` acts on all bits of a cell at once.

| Operation | Sequence (classes: bits, head) | Ops |
|---|---|---|
| `xor` | `SET 11; OPR Z; OPR Z; OPR X; OPR Z; SET 11; OPR Y; OPR Z; SET 11; OPR Z` | 10 |
| `and` | `SET 20; OPR Z; SET 11; OPR X; OPR Z; OPR Z; SET 00; OPR Y; OPR Z; SET 11; OPR Y` | 11 |
| `mov` (X touched once) | `SET 11; OPR Z; OPR Z; OPR X; OPR Z; SET 11; OPR Z` | 7 |
| `mov` (Z touched 3×, for stores) | `SET 11; OPR Z; OPR Z; SET 00; OPR X; OPR Z; SET 11; OPR X` | 8 |
| column sum `S := sigma(x+y)`, sigma = (1, 2, 0) | `SET 20; OPR X; OPR S; SET 11; OPR Y; OPR S; SET 21; OPR X; OPR S` | 9 |
| reset any cell to `C1` | `SET 11; OPR S; OPR S` | 3 |
| lane index, byte cell | `SET 2112; OPR S; OPR I; SET 2002; OPR S` (lane, fh, fa, fb) | 5 |
| lane index, word cell | `SET 22000; OPR S; SET 01002; OPR I; SET 22000; OPR S; OPR I; SET 20022; OPR I` (lane, other, fh, fa, fb) | 9 |
| pointer from an address | reset P; `SET K; OPR X; OPR P; SET R; OPR X` (`gen.Bench.ptr_consts`) | 8 |

The store `mov` and the pointer sequence were derived by hand (the search space was too large) and
are checked by simulation in `gen.py`. Overwriting a cell of unknown f02 value takes three `Opr`s on
it here. A case analysis of the `crz` table rules out two `Opr`s with nothing in between or with
one register op in between. Two touches with longer sequences in between were not ruled out.

## Results

See the ADR for the table and the decision. `out/result_L*.txt` holds the raw output of the last run.
