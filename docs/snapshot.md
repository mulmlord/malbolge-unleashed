# Startup snapshots

A startup snapshot lets `malm run` skip the steps a program executes before it first reads,
writes or halts, by restoring the VM state those steps leave (issue #38). It is an accelerator and
nothing else: a run from the snapshot is the run without it, step for step, from the state it
restores on. Implementation: `src/container/snapshot.cpp` (format), `Vm::run_pure`
(`src/core/vm.hpp`), `src/driver/snapshot_cmd.cpp` (CLI); tests: `tests/container/snapshot_test.cpp`,
`tests/driver/cli_test.cpp`, and every `malm build` sample in `tests/rv/build_pipe_test.sh`.

```sh
malm snapshot prog.malm          # writes prog.malm.snap
malm run prog.malm               # starts from prog.malm.snap
malm snapshot --check prog.malm  # runs the steps and compares with the snapshot
```

## What it holds

The steps before a program's first `In`, `Out` or `Hlt` depend on nothing but the program: they
read no input and write no output, so they reach the same state on every run. `Vm::run_pure`
runs them and stops before the first such step (or at a fault, or after `--max-steps N`). A
snapshot is the state at that point:

- the registers A, C and D, the step count and n (spec 2.2);
- every materialised memory page (3^8 cells), as a difference from the memory the program loads
  with (the program's cells, then the fill pattern).

For programs that `malm asm`, `malm hla` and `malm build` produce this covers all of the startup
code (asm.md 7, rv.md 8) and the code after it up to the first I/O: the emu-mode `hello.c`, with
1 MiB of guest RAM, runs 225 429 287 of its 226 073 739 steps before its first `Out`, and its
snapshot is 47 KB ([benchmarks.md](benchmarks.md)). The startup code of such a program executes
every cell of the program once, so most of the difference is "this cell was encrypted once".

## Guarantees

- **Identical state.** Restoring gives exactly the state that running the steps gives: the same
  registers, step count and n, the same set of materialised pages and every cell in them. Step
  numbers in fault diagnostics are therefore the same; so is the `pages` count of `--stats`.
- **Tied to one program file.** The snapshot carries the SHA-256 of the program file, byte for
  byte, and its cell count. `malm run` uses it only if both match the file it loads. The same
  program in another file (a `.malm` and its text, a different codec, added whitespace) has
  other bytes and needs a snapshot of its own; a program rebuilt after its snapshot was written
  no longer matches it.
- **The program stays pure.** The `.malm` format is unchanged and never carries data (spec 4).
  The snapshot is a separate file next to it; deleting it changes only how long a run takes.
- **Checkable.** `malm run` trusts a snapshot that is valid and matches, as it trusts the program
  file next to it: checking it would mean running the steps it saves. A snapshot that holds
  another state (edited, or written by a faulty `malm`) would change what the program does.
  `malm snapshot --check` runs the program to the snapshot's step without it and compares the two
  states in full; the e2e tests run it on every sample.

`malm run` does not use a snapshot with `--no-snapshot`, with `--trace` or `--stats` (they observe
every step), for a classic-mode program, or when there is none. A snapshot it cannot use (not
readable, invalid, made for another program file) is ignored with a note on stderr, `malm: note:
ignoring snapshot 'prog.malm.snap': <reason>`, and the run starts from the beginning. Packed
executables (`malm pack`, `malm build --exe`) run without snapshots: their payload is a `.malm`
only.

## File format (version 1)

`<program file>.snap`. All integers are little-endian. A fixed 112-byte header, then the payload.

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `MALMSNAP` |
| 8 | 2 | format version, `1` |
| 10 | 1 | mode, `0` (unleashed; there are no classic-mode snapshots) |
| 11 | 1 | reserved, 0 |
| 12 | 4 | CRC-32 (as in malm-format.md) of the decompressed body |
| 16 | 32 | SHA-256 of the program file |
| 48 | 8 | the program's cell count `L` |
| 56 | 8 | step count |
| 64 | 8 | A, the raw 40-trit value (`Value::raw`, below 3^40) |
| 72 | 8 | C, raw |
| 80 | 8 | D, raw |
| 88 | 1 | n |
| 89 | 7 | reserved, 0 |
| 96 | 8 | page count `P` |
| 104 | 8 | payload length in bytes |

The payload is exactly one zstd frame (level 9 when written) that declares its content size; the
content is the body.

**Body.** For each of the `P` materialised pages, in ascending order of page index (page `i` holds
the raw addresses `i * 3^8 .. i * 3^8 + 3^8 - 1`): the page index minus the previous page's index
minus 1 (the first page: its index), then runs that cover the page's 3^8 cells in order. A run is
one token `(length - 1) * 8 + kind`, with

| kind | the run's cells hold |
|---|---|
| 0 | what the program loads there |
| 1 .. 6 | that, encrypted 1 to 6 times (spec 5, step 4); the loaded value must be a cell, 33..126 |
| 7 | the values that follow the token, one per cell |

All numbers in the body are unsigned LEB128 varints (7 bits per byte, least significant first; at
most 64 bits). What the program loads at address `a` is its cell `a` for `a < L` (head 0) and the
fill pattern `P[a mod 6]` everywhere else (spec 2.4).

**Validation.** A reader rejects the file, naming the cause, for: a wrong magic or a short header;
another version, mode or nonzero reserved bytes; a hash or cell count that is not the program's;
an n below the program's initial n or above 39; a register value of 3^40 or more; C or D wider than
`n - 1` (spec 2.2); a page count above 3^32; a payload length other than the bytes after the
header; a frame that is not exactly one frame, declares no content size, declares more than `P`
pages can need, or does not decompress to its declared size; a CRC mismatch; and a body that is
truncated or has data after the last page, a malformed varint, a page index past the last page
(3^32 - 1), a missing program page (pages `0 .. ceil(L / 3^8) - 1` hold the program and are always
materialised), a run past the end of its page, an encrypted run over a cell that is not 33..126,
or a literal of 3^40 or more. The decompressed body is bounded by `P` pages of literals, and memory
grows only as data arrives.

## `malm snapshot`

```
usage: malm snapshot [--max-steps N] [--max-cells N] [--] <file>
       malm snapshot --check [--max-cells N] [--] <file>
       (writes or checks <file>.snap)
```

- **Writing** runs the program to its first `In`, `Out` or `Hlt` (at most `--max-steps N` steps,
  a positive integer), writes `<file>.snap` and prints `<file>.snap: S steps (up to the first
  Out), P pages, B bytes` (or `(--max-steps)`) on stdout. A program that faults before its first
  I/O has nothing worth saving: `malm snapshot: the program faults before its first I/O (E_xxx at
  step ...); no snapshot written`, exit 1.
- **`--check`** reads and validates `<file>.snap`, runs the program to the snapshot's step without
  it and compares registers, step count, n, the materialised pages and every cell: `<file>.snap:
  matches the program at step S` (exit 0), or `malm snapshot: '<file>.snap' does not match the
  program at step S: <the first difference>` (exit 1), for example `A is 0t..., the snapshot has
  0t...`, `cell 0t... is ..., the snapshot has ...` or `the program does I/O first, at step ...`.
- `--max-cells` is the reader cap of `malm run`. Classic-mode programs are refused (exit 1).
  Usage errors exit 2.
