# aot-mode benchmarks

The size and speed of programs built in aot mode ([rv.md](rv.md) section 9), tracked from the
first version of rv-aot (issue #29) on. `tests/rv/aot_bench.sh` builds every program of the
three-way differential test (rv.md section 7) with `malm build --mode aot`, runs each on its first
input and prints the table rows:

```sh
sh tests/rv/aot_bench.sh build/release/src/driver/malm build/release/tests/rv/elf
```

- **`.malm` bytes:** the file `malm build` writes (zstd-compressed, spec 4).
- **Malbolge text:** the length of the program text (`malm export --text`), which is the startup
  code; the startup runs about as many steps.
- **VM steps:** `malm run --stats`, startup included, on the program's input from the
  differential test (`runtime` on `runtime.in`). Steps do not depend on the machine; wall-clock
  times on the development machine vary by up to 10x, so they are only given measured back to
  back.

The programs are the differential test's, which dump their registers as they exit (ecall 255,
rv.md section 4): the bare `hello.c` and `cat.c` without the runtime, the C examples and the
runtime's self-check on it.

## The first version (commit 0cdab20, issue #30)

| program | guest RAM | .malm bytes | Malbolge text | VM steps |
|---|---|---|---|---|
| hello | 64K | 624259 | 24355979 | 25316566 |
| cat | 64K | 674564 | 26965889 | 27962810 |
| example_hello | 64K | 1183514 | 43046751 | 43522772 |
| example_cat | 64K | 1287245 | 43046751 | 44979168 |
| wc | 64K | 4199282 | 106941653 | 115060258 |
| fizzbuzz | 64K | 4875248 | 129486425 | 152723414 |
| sort | 64K | 2635473 | 70886825 | 85270024 |
| runtime | 256K | 14759338 | 387420545 | 6937119606 |
| total | | 30238923 | 832150818 | 7431954618 |

## Outlining and constants (issue #31)

Two changes, in HLA and in rv-aot:

- **Outlining** (hla.md 4.8): a statement that occurs more than once is lowered once, as a routine,
  and called. A real program repeats most of its statements (`mov pc, ...` and `mov lkm, ...`
  before memory accesses are the exception), so this removes most of the program text. It also
  runs faster: the registers' return trees shrink. Alone it took the total to 14 178 269 bytes
  (-53%) and 451 M characters (-46%).
- **Constants** (rv.md section 9): arithmetic on constants is done at build time, register writes
  that the block overwrites unread are left out, and loads and stores at known addresses need no
  range check and no `mov pc` (an aligned word is one HLA `load` or `store`).

| program | guest RAM | .malm bytes | Malbolge text | VM steps |
|---|---|---|---|---|
| hello | 64K | 473497 | 21112261 | 22026101 |
| cat | 64K | 470323 | 22695563 | 23644066 |
| example_hello | 64K | 707798 | 28224661 | 31503966 |
| example_cat | 64K | 731936 | 27743773 | 30971641 |
| wc | 64K | 2047200 | 61429579 | 68803404 |
| fizzbuzz | 64K | 2203397 | 64430341 | 85331731 |
| sort | 64K | 1220165 | 38750669 | 51708020 |
| runtime | 256K | 5455146 | 167031469 | 6212550436 |
| total | | 13309462 | 431418316 | 6526539365 |

Against the first version: `.malm` -56% (from -24% for the bare hello to -63% for the self-check),
Malbolge text -48%, VM steps -12%. The larger the program, the larger the gain: the runtime's
fixed part (guest RAM, the dispatch table, the switch and ALU tables, the register dump) does not
shrink. The runs that are mostly startup (all but `runtime`) save what the startup saves; the
self-check, which runs 3.1 M guest instructions, saves 10% of its steps.

Steady state, fizzbuzz on the input 2000 (848 167 guest instructions): 3.223 G steps in the first
version, 2.864 G now (-11%), the same output. Building, back to back on one machine: fizzbuzz 23.7 s
then, 8.2 s now; the self-check 84 s then, 21 s now. Outlining applies to every HLA program, and
rv-emu gains too: fizzbuzz built in emu mode with 64 KiB of guest RAM went from 1.03 MB to
0.77 MB of `.malm`, and its run on 2000 from 8.466 G to 8.002 G steps (-5.5%).
