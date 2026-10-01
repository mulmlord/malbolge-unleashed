# The `.malm` container format (version 1)

A `.malm` file holds one Malbolge program: its cells, the mode it runs in and one flag. It is pure
loading data: **the container never carries data** (no preloaded memory; spec 4, design spec 2.5).
Implementation: `src/container/`; tests: `tests/container/container_test.cpp`.

All integers are little-endian. A file is a fixed 32-byte header followed by the payload.

## Header

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `MALM` (`4D 41 4C 4D`) |
| 4 | 2 | format version, `1` |
| 6 | 1 | mode: `0` = unleashed, `1` = classic |
| 7 | 1 | flags: bit 0 = `EXIT_A`; all other bits must be 0 |
| 8 | 1 | codec: `0` = packed (no compression), `1` = zstd(packed) |
| 9 | 3 | reserved, must be 0 |
| 12 | 4 | CRC-32 (IEEE 802.3, reflected, polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`) of the **packed, uncompressed** symbol bytes |
| 16 | 8 | cell count `L` |
| 24 | 8 | payload length in bytes |

`EXIT_A`: when set, a halting program exits with `A mod 256` instead of 0 (spec 7). `malm run
--exit-a` enables the same behaviour (logical OR with the flag).

`L` must satisfy `2 <= L <= max`, where `max` is the loader's limit for the mode: `3^38` for
unleashed, `59049` for classic. The magic can never begin a valid text program, because `M` is not
loadable at cell 0, so `.malm` and text are told apart by the first four bytes alone.

## Normalization

A cell with code `x` (33..126) at position `i` normalizes to its instruction letter
`xlat1[(x - 33 + i) mod 94]`, which is one of `j i * p < / v o`. The letter is stored as a 3-bit
symbol, its index in the string `ji*p</vo`:

| symbol | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| letter | `j` | `i` | `*` | `p` | `<` | `/` | `v` | `o` |
| instruction | MovD | Jmp | Rot | Opr | Out | In | Hlt | Nop |

Denormalization is the inverse (research 1.2): for the instruction code `k` of the letter (the `k`
with `xlat1[(k - 33) mod 94]` equal to the letter) and position `p`,

    ch = (k - p mod 94 + 94) mod 94;  if ch < 33 then ch += 94

For every position exactly 8 of the 94 byte values 33..126 are loadable and they map to 8 distinct
letters, so the mapping is a bijection and `denormalize(normalize(x)) == x` (checked for all 94
positions in the tests). Text to `.malm` to text therefore reproduces the program text exactly,
apart from whitespace, which the loader ignores.

## Packing

Symbols are packed 3 bits per cell, least significant bit first: cell `i` occupies bits
`3i .. 3i+2` of the byte stream (bit `b` is bit `b mod 8` of byte `b / 8`). The packed size is
`ceil(3L / 8)` bytes. Unused trailing bits of the last byte must be 0.

## Payload

* codec 0: the packed bytes; the payload length must equal `ceil(3L / 8)`.
* codec 1: exactly one zstd frame (level 9 when written) whose decompressed content is the packed
  bytes. The frame must declare its content size, and it must equal `ceil(3L / 8)`. Nothing may
  follow the frame (no second frame, no skippable frame, no other data): the frame's compressed
  size must equal the payload length.

The default codec of the writer is 1. Codec 0 exists for debugging and tests.

## Validation on read

A reader rejects the file, with a message that names the cause, for each of:

1. wrong magic, or a file shorter than the 32-byte header
2. unsupported format version
3. unknown mode, unknown codec
4. unknown flag bits, nonzero reserved bytes
5. `L < 2` or `L` above the limit of the mode
6. payload length different from the bytes that follow the header (truncated file, trailing data)
7. codec 0: payload length different from `ceil(3L / 8)`
8. codec 1: frame without a content size, content size different from `ceil(3L / 8)` (checked
   before decompressing), data after the first frame, or a frame that fails to decompress or
   produces more than the declared size
9. CRC mismatch
10. nonzero padding bits

## Reader policy (not part of the format)

The format allows up to `3^38` cells, and a tiny zstd payload can legitimately describe billions of
`Nop` cells (the ratio approaches 32768 : 1). So a reader enforces a resource cap of its own:

* `malm` rejects `L` above **2^30 cells** by default, comparing the header's `L` **before**
  decompressing or allocating anything: `program has N cells, above the limit M (use --max-cells)`.
  `--max-cells N` on `malm run` and `malm export` changes the cap (N positive and at most the
  format limit of the mode: `3^38`, or `59049` with `--classic`). It applies to plain text input
  too. For a classic `.malm` the format limit of 59049 applies in any case.
* zstd payloads are decompressed as a stream in 1 MiB chunks, unpacked and CRC-checked on the fly,
  and the cell array grows only as real data arrives; memory is bounded by the data actually
  present, never by what the header claims. A corrupt or hostile payload fails after at most one
  chunk. The reader also refuses zstd windows above 2^24 bytes (level 9 writes 2^21).
* Running out of memory anyway prints `malm: out of memory` and exits 1.

Container errors print as `malm: E_LOAD: invalid .malm '<path>': <reason>` (exit 1).

## Text export

`malm export --text` writes the denormalized cells with no whitespace and one trailing `\n`. The
output is loader-compatible (the loader skips whitespace) and equals the original program text
with all whitespace removed, plus the final newline.

Plain text cannot represent the mode or `EXIT_A`. When the input `.malm` is classic or has `EXIT_A`,
`export --text` says so on stderr (`malm: warning: plain text cannot represent classic mode or
EXIT_A; not exported`) and still writes the text.

## CLI

    malm run [--classic] [--exit-a] [--max-cells N] [--] <file>
    malm export (--text|--malm) [--classic] [--exit-a] [--codec packed|zstd]
                [--max-cells N] -o <out> [--] <in>

Options come before `--`; after it every word is an input name. `-o -` writes to stdout. `run`
detects `.malm` by the magic; the header's mode decides, `--classic` with an unleashed `.malm` is a
usage error (exit 2), and `--exit-a` is OR-ed with the header's `EXIT_A`.
