# VM and loading benchmarks

The speed of the VM's step loop, the interpreter that every program runs on: hand-written and
assembled Malbolge, HLA programs, and RISC-V programs in emu and aot mode (issue #37). Program size
and the steps of aot mode are tracked in [aot-benchmarks.md](aot-benchmarks.md). Loading a program
and its `.malm` compression are measured in [issue #39](#issue-39-compression).

## How to run

`tests/bench/vm_bench.sh` builds the benchmark programs with `malm` and times each with `vm_bench`
(`tests/bench/vm_bench.cpp`, built with the tests), printing the table rows:

```sh
cmake --build --preset release --target malm vm_bench
sh tests/bench/vm_bench.sh build/release/src/driver/malm build/release/tests/bench/vm_bench
```

Given a third argument, a `vm_bench` built from an older commit, the script runs every program on
both, alternately, and prints the speed-up. `vm_bench` only uses `load_program` and
`BasicVm::run`, so it builds on older commits as it is:

```sh
git worktree add --detach ../base 541f3f0
cp -r tests/bench ../base/tests/ && echo 'add_subdirectory(bench)' >> ../base/tests/CMakeLists.txt
(cd ../base && cmake --preset release && cmake --build --preset release --target vm_bench)
sh tests/bench/vm_bench.sh build/release/src/driver/malm build/release/tests/bench/vm_bench \
    ../base/build/release/tests/bench/vm_bench
```

- **What is timed:** `Vm::run` (`ClassicVm::run` in classic mode) without an observer, the loop
  `malm run` runs. Reading and decompressing the file and building the VM's memory are not timed;
  `vm_bench` prints the time of the latter separately. Input comes from memory, output is counted
  and dropped.
- **steps/s:** VM steps divided by the time of the fastest run. `vm_bench` runs a program again
  until it has stepped for one second in total; the script calls it three times per program and
  takes the fastest. `malm run --stats` reports a lower rate, with its observer attached.
- **Noise:** the development machine's clock varies a lot under load (up to 10x), so the rates are
  only comparable within one table, measured back to back. VM steps do not depend on the machine.

The programs:

- **Classic:** `beer99.mal`, the 99-bottles program of the classic corpus (`tests/corpus`); the
  corpus' other programs run a few hundred steps.
- **asm:** the examples of `examples/asm` (`malm asm`), on short inputs (`123 456`, `9875`,
  `Hello, cat!`).
- **HLA:** the examples of `examples/hla` (`malm hla`).
- **C:** `examples/c/hello.c` built for emu mode (`malm build`, 1 MiB of guest RAM) and for aot mode
  (`--mode aot`), and `fizzbuzz.c` in emu mode on `tests/rv/inputs/fizzbuzz.in` (15). Emu-mode runs
  are mostly startup (spec 4), which writes rv-emu's registers, RAM and tables.

## Issue #37 against M3

The VM as of M3 (commit 541f3f0; `src/core` was last changed in M1) against this version, back to
back on one machine (AMD Ryzen 7 7840HX, clang 22, release preset):

| program | mode | VM steps | steps/s before | steps/s after | speed-up |
|---|---|---|---|---|---|
| beer99 | classic | 13802606 | 274.0 M | 393.6 M | 1.44x |
| asm hello | unleashed | 6980 | 44.7 M | 145.9 M | 3.26x |
| asm adder | unleashed | 166690 | 39.2 M | 151.8 M | 3.88x |
| asm digital_root | unleashed | 48861 | 40.3 M | 149.6 M | 3.72x |
| asm halting_cat | unleashed | 13202 | 40.4 M | 140.7 M | 3.48x |
| hla hello | unleashed | 42471 | 37.2 M | 154.6 M | 4.16x |
| hla rev | unleashed | 3194233 | 32.6 M | 94.3 M | 2.90x |
| hla cat | unleashed | 1216291 | 38.6 M | 154.3 M | 4.00x |
| C hello, emu | unleashed | 226073739 | 31.6 M | 85.4 M | 2.71x |
| C fizzbuzz, emu | unleashed | 273423794 | 32.1 M | 90.2 M | 2.81x |
| C hello, aot | unleashed | 215063793 | 31.2 M | 83.0 M | 2.66x |
| total | | 733052660 | 32.2 M | 87.8 M | 2.73x |

Unleashed mode is 2.7 to 4.2 times as fast, emu and aot mode 2.7 to 2.8 times; the total, all steps
over all seconds, is 2.7 times, above the issue's 2x. Classic mode gains 1.4 times (below).

### What changed

The step loop (`src/core/vm.cpp`) keeps the order of spec 5; the work per step shrank:

1. **Registers in registers.** The loop runs on a local copy of A, C, D and the step counter, and
   the step is always inlined into it. Before, clang inlined the step in classic mode but not in
   unleashed mode, and the registers were members, reloaded after every memory write (a `Value`
   cell can alias them).
2. **C mod 94 kept, not computed.** The remainder rule is continuous under +1 except at the wrap
   to 0, so the loop counts it on and recomputes it only after a jump or the wrap. Decoding is
   one lookup, `kOpOfSum[C mod 94 + [C]]`, without a modulo, and the `switch` on the op has no
   range check.
3. **Cheaper checks.** The growth rule tests head 0 and head 2 registers with one comparison each
   against limits kept for the current n (3^(n-1) and 3^40 - 3^(n-1)); the increment compares
   with the two raw values where the head would change, instead of dividing.
4. **Page cursors** (`src/core/memory.hpp`). C and D each keep the page they are in: while a
   register stays in its page, an access is one subtraction and comparison. Behind the cursors,
   a 64-slot direct-mapped page cache replaces the two-entry MRU cache, which swapped its entries
   whenever C and D alternated.
5. **Shared fill pages.** A page that was never written reads from one of two shared pages of the
   fill pattern: the pattern index of an address is `raw % 6` (`Value::mod6() == raw() % 6`, a
   property tested in `value_test`), and pages start at phase 0 or 3. A write copies the shared
   page instead of zeroing a page and filling it cell by cell; building the memory of the emu-mode
   hello (about 100 000 pages) went from 0.95 s to 0.72 s.
6. **[C] read once.** Encryption reads [C] again only after a jump, or after Rot or Opr with
   D = C; otherwise it is still the fetched cell, already checked.

Their effect, in steps/s, the median of three runs of two seconds each, measured back to back
(each row adds changes to the one above it):

| version | C hello, emu | beer99, classic |
|---|---|---|
| M3 | 30 M | 264 M |
| 1, 2, 3, 5 and the 64-slot page cache | 67 M | 413 M |
| 4, page cursors | 73 M | 402 M |
| 6, [C] read once (this version) | 81 M | 386 M |

Tried and dropped: `rotr` without its division by the run-time 3^n (subtracting the head trits
instead) ran fewer instructions at the same speed. The last two rows were measured with it in;
this version without it measured the same, alternating with it.

`tests/core/vm_test` checks the loop against a plain reference step (the machine's plain read and
write, C mod 94 recomputed and [C] read again every step) on random programs in both modes, also
across the `...222 -> 0` wrap; `memory_test` checks the cursors against plain accesses.

### What did not change

- **Classic mode** gains about 1.4x. Its memory was always a flat array of 16-bit cells, the loop
  was already 6 to 9 times as fast as in unleashed mode, and beer99 is 53% jumps, each a load of
  [D] and then of the new [C]. The [C]-once change costs it about 4% and gains emu mode about 10%.
- **Dispatch.** The `switch` stays (computed goto would need a compiler extension). The loop
  mispredicts a branch on 12% of the emu-mode steps (about 3% of the cycles) and on 3% of
  beer99's.
- **Cells of 8 bytes.** Much of an emu-mode step is the load of [C] and the dispatch that depends
  on it, in a memory of about 100 000 pages (5 GB with 1 MiB of guest RAM, [rv.md](rv.md)).
  Denser pages (most cells hold 33..126) would need a second page format for the rest; not done.
- **Startup** is steps like any others here; the startup snapshot (below) skips them.

## Issue #38: startup snapshots

A startup snapshot ([snapshot.md](snapshot.md)) replaces the steps before a program's first I/O
by restoring the state they leave. Measured in process with `Vm::run_pure`, `write_snapshot`,
`Snapshot::read` and `restore`, the fastest of three runs each (same machine and build as above):

| program | steps before the first I/O | of all steps | pages | snapshot | the steps | restoring | speed-up |
|---|---|---|---|---|---|---|---|
| hla hello | 42 269 | 42 471 | 35 | 743 B | 0.001 s | under 0.001 s | |
| hla rev | 3 177 408 | 3 194 233 | 1 283 | 7.9 KB | 0.037 s | 0.005 s | 7x |
| C hello, emu, 64K | 37 349 343 | 37 993 705 | 10 673 | 46.7 KB | 0.398 s | 0.042 s | 9.5x |
| C hello, emu, 1M | 225 429 287 | 226 073 739 | 100 778 | 47.3 KB | 1.942 s | 0.423 s | 4.6x |
| C fizzbuzz, emu, 1M | 216 951 642 | 273 423 794 | 99 895 | 56.2 KB | 1.870 s | 0.364 s | 5.1x |
| C hello, aot, 1M | 214 848 406 | 215 063 793 | 99 566 | 47.9 KB | 1.818 s | 0.372 s | 4.9x |

"Restoring" is reading the snapshot (validation and decompression: 11 ms for the 1 MiB programs)
and writing its pages into a VM that has loaded the program. The startup code of these programs
runs every program cell once, so the snapshot of a 218 M-cell program is a few runs per page: the
program encrypted once, about 880 000 other cells (rv-emu's registers and guest RAM, the tables),
and 67 000 pages beyond the program. Restoring is mostly materialising those pages (3.5 GB) and
encrypting the program's cells in place; the steps themselves are gone. Writing a snapshot costs
about 1.1 s for the 1 MiB programs, once.

`malm run` as a whole, with and without the snapshot (`--no-snapshot`), the fastest of three
runs alternating the two, output to `/dev/null`:

| program | without | with | speed-up |
|---|---|---|---|
| hla rev | 0.062 s | 0.044 s | 1.41x |
| C hello, emu, 64K | 0.614 s | 0.377 s | 1.63x |
| C hello, emu, 1M | 4.332 s | 3.031 s | 1.43x |
| C fizzbuzz, emu, 1M | 4.679 s | 3.421 s | 1.37x |
| C hello, aot, 1M | 4.217 s | 2.972 s | 1.42x |

The snapshot saves what the steps cost beyond restoring, 1.2 to 1.3 s of a whole run of the
1 MiB programs; the rest of a run is now loading. Profiled with the snapshot, 54% of `malm run` of the 1 MiB hello is unpacking the
`.malm` (`Unpacker::feed`, 218 M cells denormalized one by one into a vector that grows by
doubling), then the restore (9%) and building the VM's memory (4%), with the kernel's page faults
for 5 GB of fresh memory and freeing it at exit. Loading faster is issue #39 (below).

## Issue #39: compression

How small a `.malm` gets and how fast `malm` loads it, for programs from 166 000 to 800 M cells.
`tests/bench/codec_bench.sh` builds the programs, compresses each in process with `codec_bench`
(`tests/bench/codec_bench.cpp`) and times loading the `.malm` of each codec:

```sh
cmake --build --preset release --target malm codec_bench
sh tests/bench/codec_bench.sh build/release/src/driver/malm build/release/tests/bench/codec_bench
```

A third argument, a `codec_bench` built from an older commit (copy `tests/bench` into it, as for
`vm_bench` above), adds its read time of the codec-1 file to the second table. The tables below were
measured that way against commit 7599ba7, on the machine and build of the sections above (clang 22
for both: a GCC build of the old commit reads at the same speed but runs the VM 30% slower).

### What is compressed

The cells are normalized ([malm-format.md](malm-format.md)): 3 bits each, 8 instructions. What
compresses is the layout of those symbols:

- **packed:** 3 bits per cell, the codec-1 payload (`ceil(3L / 8)` bytes);
- **2 cells per byte:** 6 bits per byte;
- **1 cell per byte:** the codec-2 payload, `L` bytes.

The 1 MiB C programs are about 2.4 % of their packed size at level 9 already: the startup code
writes rv-emu's RAM, registers and tables with the same few instruction sequences, millions of
times. What decides the ratio is how well zstd finds those repeats. In the packed layout a sequence
that recurs at a distance that is not a multiple of 8 cells starts at another bit, which zstd,
matching bytes, sees as other data; one cell per byte has no such offsets.

### Ratio

Compressed size, as a share of the packed size; compression time single-threaded (except "as
written", 8 workers as `malm` writes codec 2), decompression time the fastest of three, streamed in
1 MiB chunks as the reader does. xz is the `xz` command, `-9e -T1`, timed with its process start.

| program | compression | bytes | of packed | compress | decompress |
|---|---|---|---|---|---|
| asm adder | packed, zstd 9 (codec 1) | 13627 | 21.9% | 0.00 s | 0.000 s |
| asm adder | packed, zstd 19 | 12086 | 19.4% | 0.02 s | 0.000 s |
| asm adder | 2 cells per byte, zstd 19 | 8834 | 14.2% | 0.03 s | 0.000 s |
| asm adder | 1 cell per byte, zstd 12 | 9779 | 15.7% | 0.02 s | 0.000 s |
| asm adder | 1 cell per byte, zstd 19 (codec 2) | 8348 | 13.4% | 0.08 s | 0.000 s |
| asm adder | packed, xz -9e | 12060 | 19.4% | 0.02 s | 0.000 s |
| asm adder | 1 cell per byte, xz -9e | 8472 | 13.6% | 0.05 s | 0.000 s |
| hla rev | packed, zstd 9 (codec 1) | 92734 | 7.8% | 0.01 s | 0.000 s |
| hla rev | packed, zstd 19 | 72276 | 6.1% | 0.29 s | 0.000 s |
| hla rev | 2 cells per byte, zstd 19 | 49874 | 4.2% | 0.30 s | 0.000 s |
| hla rev | 1 cell per byte, zstd 12 | 82001 | 6.9% | 0.02 s | 0.000 s |
| hla rev | 1 cell per byte, zstd 19 (codec 2) | 47868 | 4.0% | 0.34 s | 0.000 s |
| hla rev | packed, xz -9e | 71080 | 6.0% | 0.27 s | 0.000 s |
| hla rev | 1 cell per byte, xz -9e | 45332 | 3.8% | 0.57 s | 0.000 s |
| C hello, emu, 1M | packed, zstd 9 (codec 1) | 2389091 | 2.9% | 0.31 s | 0.012 s |
| C hello, emu, 1M | packed, zstd 19 | 1634455 | 2.0% | 26.23 s | 0.010 s |
| C hello, emu, 1M | 2 cells per byte, zstd 12 | 1846608 | 2.3% | 0.48 s | 0.011 s |
| C hello, emu, 1M | 2 cells per byte, zstd 19 | 1606310 | 2.0% | 18.60 s | 0.011 s |
| C hello, emu, 1M | 1 cell per byte, zstd 12 | 2144537 | 2.6% | 0.52 s | 0.015 s |
| C hello, emu, 1M | 1 cell per byte, zstd 9, long, 2^27 | 2005541 | 2.5% | 0.67 s | 0.025 s |
| C hello, emu, 1M | 1 cell per byte, zstd 19 (codec 2) | 1378031 | 1.7% | 14.59 s | 0.015 s |
| C hello, emu, 1M | 1 cell per byte, zstd 19, as written | 1390150 | 1.7% | 4.05 s | 0.015 s |
| C hello, emu, 1M | 1 cell per byte, zstd 19, 2^27 | 1285239 | 1.6% | 17.00 s | 0.023 s |
| C hello, emu, 1M | packed, xz -9e | 1352640 | 1.7% | 48.92 s | 0.050 s |
| C hello, emu, 1M | 1 cell per byte, xz -9e | 1311520 | 1.6% | 26.08 s | 0.070 s |
| C fizzbuzz, emu, 1M | packed, zstd 9 (codec 1) | 2213425 | 2.8% | 0.28 s | 0.011 s |
| C fizzbuzz, emu, 1M | 2 cells per byte, zstd 19 | 1376807 | 1.7% | 18.78 s | 0.010 s |
| C fizzbuzz, emu, 1M | 1 cell per byte, zstd 19 (codec 2) | 1427266 | 1.8% | 14.01 s | 0.015 s |
| C fizzbuzz, emu, 1M | 1 cell per byte, zstd 19, as written | 1423239 | 1.8% | 4.10 s | 0.015 s |
| C fizzbuzz, emu, 1M | 1 cell per byte, zstd 19, 2^27 | 1287054 | 1.6% | 16.52 s | 0.025 s |
| C fizzbuzz, emu, 1M | packed, xz -9e | 1238000 | 1.6% | 46.44 s | 0.050 s |
| C hello, aot, 1M | packed, zstd 9 (codec 1) | 2073219 | 2.6% | 0.27 s | 0.011 s |
| C hello, aot, 1M | 2 cells per byte, zstd 19 | 1310553 | 1.6% | 18.38 s | 0.010 s |
| C hello, aot, 1M | 1 cell per byte, zstd 19 (codec 2) | 1338685 | 1.7% | 13.56 s | 0.014 s |
| C hello, aot, 1M | 1 cell per byte, zstd 19, as written | 1360535 | 1.7% | 4.12 s | 0.015 s |
| C hello, aot, 1M | 1 cell per byte, zstd 19, 2^27 | 1202195 | 1.5% | 16.09 s | 0.024 s |
| C hello, aot, 1M | packed, xz -9e | 1172500 | 1.5% | 46.12 s | 0.040 s |
| C hello, emu, 4M | packed, zstd 9 (codec 1) | 9395410 | 3.1% | 1.19 s | 0.045 s |
| C hello, emu, 4M | packed, zstd 19 | 5629654 | 1.9% | 87.06 s | 0.033 s |
| C hello, emu, 4M | 2 cells per byte, zstd 19 | 4803666 | 1.6% | 61.77 s | 0.034 s |
| C hello, emu, 4M | 1 cell per byte, zstd 9, long, 2^27 | 5686316 | 1.9% | 2.55 s | 0.073 s |
| C hello, emu, 4M | 1 cell per byte, zstd 19 (codec 2) | 4586196 | 1.5% | 37.44 s | 0.050 s |
| C hello, emu, 4M | 1 cell per byte, zstd 19, as written | 4602943 | 1.5% | 8.11 s | 0.051 s |
| C hello, emu, 4M | 1 cell per byte, zstd 19, 2^27 | 4741053 | 1.6% | 45.61 s | 0.078 s |
| C hello, emu, 4M | packed, xz -9e | 4743388 | 1.6% | 144.71 s | 0.180 s |
| C hello, emu, 4M | 1 cell per byte, xz -9e | 4244692 | 1.4% | 87.09 s | 0.270 s |

(The script prints a few more rows per program, all between their neighbours here.) Zstd's own
`--ultra -22 --long=27` reached 1.06 MB for the aot hello, one cell per byte, in 224 s.

### Load time

Reading the `.malm` (`read_malm`: decompressing, unpacking, the CRC; the fastest of three) of
codec 1 before this issue and of both codecs after it, and building the VM's memory from the cells
(unchanged, for comparison):

| program | cells | codec 1 | codec 2 | read before (codec 1) | read, codec 1 | read, codec 2 | memory |
|---|---|---|---|---|---|---|---|
| asm adder | 165755 | 13659 B | 8380 B | 0.001 s | 0.000 s | 0.000 s | 0.000 s |
| hla rev | 3177309 | 92766 B | 47900 B | 0.010 s | 0.002 s | 0.003 s | 0.003 s |
| C hello, emu, 1M | 217797733 | 2389123 B | 1390182 B | 0.720 s | 0.158 s | 0.221 s | 0.193 s |
| C fizzbuzz, emu, 1M | 211994455 | 2213457 B | 1423271 B | 0.705 s | 0.146 s | 0.214 s | 0.176 s |
| C hello, aot, 1M | 212647191 | 2073251 B | 1360567 B | 0.705 s | 0.152 s | 0.213 s | 0.184 s |
| C hello, emu, 4M | 802654453 | 9395442 B | 4602975 B | 2.603 s | 0.568 s | 0.768 s | 0.694 s |

Decompressing is a small part of reading for every layout and level: 10 to 25 ms of the 1 MiB
programs' 150 to 220 ms. The rest is producing 200 M cells, which this issue made 4.5 times as
fast:

- **Unpacking 8 cells at a time** (`Unpacker` in `src/container/container.cpp`). The cell array
  grows by one resize per chunk instead of a `push_back` per cell, and 3 packed bytes, or 8
  symbols, become 8 cells through a table of the denormalized code per position and symbol, with
  rows 94..100 repeating rows 0..6 so a group never wraps the position. Before, each cell was a
  `push_back` and a division by 94.
- **The CRC 8 bytes at a time** (slicing-by-8): it took a third of the reading time after the
  first change.

Codec 2 reads in about 40% more time than codec 1: it decompresses 8 bytes for every 3 of codec 1
and packs the symbols again for the CRC. That is 60 ms for 200 M cells, against a file 34 to 42%
smaller.

`malm run` of the 1 MiB emu hello as a whole, the `malm` of commit 7599ba7 on the codec-1 file
against this one on the codec-2 file, each with its snapshot ([issue #38](#issue-38-startup-snapshots))
and without, the fastest of three runs alternating the two (the machine ran slower than for the
issue #38 table, so only the columns of this table compare):

| | before | after | speed-up |
|---|---|---|---|
| with the snapshot | 3.223 s | 2.729 s | 1.18x |
| without | 4.542 s | 4.061 s | 1.12x |

What is left of a run with the snapshot is mostly memory: restoring the snapshot's pages, building
the VM's memory of 100 000 pages and the kernel's page faults for them; unpacking is 16% of it, down
from 54%.

### The defaults

- **Codec 2, one cell per byte, zstd level 19, the level's window (2^23 for large programs).**
  34 to 42% smaller than codec 1 for the 1 MiB programs, 51% for the 4 MiB one, 39 and 48% for the
  asm and HLA programs; at the cost of compression time only, as decompression stays near 15 ms
  for 200 M cells.
- **8 worker threads.** Single-threaded, level 19 compresses the 1 MiB programs in 14 s and the
  4 MiB one in 37 s, 4 s and 8 s with 8 workers. More workers did not help on 24 cores (each
  job is 4 windows, 32 MiB); peak memory is 0.8 GB with 8 against 0.2 GB with one. zstd's output
  with workers differs from its single-threaded output by up to 2%, but is the same for any number
  of workers (tested in `container_test`), so a file does not depend on the machine that built it.
- **Not a larger window or long mode.** A 2^27 window with level 19 gains 7 to 10% on the 1 MiB
  programs and loses 3% on the 4 MiB one, compresses 15 to 20% slower and decompresses 50%
  slower; long-distance matching on top of it gave the same sizes. Either would also need the reader's window
  limit (2^24, a defence against hostile frames, [malm-format.md](malm-format.md)) raised.
- **Not 2 cells per byte.** Smaller than one per byte for fizzbuzz and the aot hello (by 2 to 4%),
  larger for the others (by up to 17%), and its compression is 30 to 65% slower.
- **Not xz.** `-9e` is 6 to 14% smaller than codec 2 on the large programs, but compresses 2 to 4
  times as slowly as single-threaded zstd (6 to 12 times as slowly as `malm` writes codec 2),
  decompresses 3 to 5 times as slowly, and would add liblzma to a statically linked binary. zstd
  stays the only compressor.
- **Not a trained dictionary.** Trained on HLA programs (68 samples of 64 KB) or on an emu-mode
  program (1618 samples of 128 KB), a dictionary saved 30 bytes on the smallest HLA program, made
  others larger (hla hello 1292 → 1566 bytes, asm adder 8352 → 9407, the 64 KiB emu hello 376 →
  446 KB), and would have to ship in every `malm` and stay fixed with the format. A program's
  repeats are its own; zstd finds them without help.
- **Codec 1 stays readable**, and `malm export --codec zstd-packed` still writes it, at level 9 as
  before, for `malm` versions without codec 2.
