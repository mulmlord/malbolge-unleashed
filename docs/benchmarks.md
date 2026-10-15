# VM benchmarks

The speed of the VM's step loop, the interpreter that every program runs on: hand-written and
assembled Malbolge, HLA programs, and RISC-V programs in emu and aot mode (issue #37). Program size
and the steps of aot mode are tracked in [aot-benchmarks.md](aot-benchmarks.md).

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
for 5 GB of fresh memory and freeing it at exit. Loading faster is issue #39.
