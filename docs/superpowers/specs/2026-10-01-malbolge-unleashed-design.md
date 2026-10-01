# malbolge-unleashed — Design Spec

Date: 2026-10-01
Status: approved in brainstorming; awaiting written-spec review
Research basis: [`docs/research/malbolge-research.md`](../../research/malbolge-research.md) (cited below as `[R §x]`)

## 1. Goal

malbolge-unleashed is a Malbolge variant with no memory limit: memory is 3^n cells, and n grows when the program needs it. The project delivers:

1. A **C++ VM** that runs malbolge-unleashed programs (`.malm`). It can also run classic Malbolge.
2. A **toolchain modelled on MalbolgeLISP** that translates *any* language which can be compiled to RISC-V (RV32IM) into malbolge-unleashed. It has two modes:
   - **emu mode** — a RISC-V emulator written in malbolge-unleashed runs the program image. Interpreted languages (MicroPython, Lua, …) are supported by running their RV32 interpreter build.
   - **aot mode** — every RISC-V instruction is translated into inline malbolge-unleashed code. This is faster but produces larger programs.
3. **Native executables**: a `.malm` packed together with the VM into one binary.
4. **stdin piping end-to-end**: `echo data | ./prog` passes stdin through the malbolge `In` instruction into the emulated program's `read()`.

Non-goals: Malbolge Unshackled compatibility (random rotation width), preloading arbitrary data into memory, and a JIT.

## 2. Language definition

Everything not listed here is the same as classic Malbolge as implemented by Olmstead's reference interpreter `[R §1]`. This includes:
- the 8 instructions decoded via `xlat1[((C + [C]) mod 94)]`
- the crz trit table
- the xlat2 encryption after each instruction
- the reference I/O mapping (`<` = output, `/` = input) `[R §1.7]`

### 2.1 Values (3-adic, as in Unshackled)
- A value is an infinite trit string whose **head trit** repeats forever to the left `[R §3.1]`. `width(v)` = the number of trits below the repeating head (`...0001` has width 1, `...000` has width 0).
- Head 0 = non-negative integers. Head 2 = negative integers (`...222` = −1). Head 1 = other 3-adic values; they are valid as data and addresses, but not as I/O.
- crz and rotation are trit-wise and head-consistent, so widening never changes a value's meaning.
- `x mod 94` and `x mod 6` for any head follow the Unshackled remainder rule `[R §3.1]`.

### 2.2 Memory: 3^n cells, n only grows
- The machine has a size parameter **n**, which starts at **10**.
- Addressable cells are all values with `width ≤ n−1`. That is 3 head-regions × 3^(n−1) = **3^n cells**.
- **Growth rule (deterministic):** after every step, `n := max(n, width(C)+1, width(D)+1)`. There is no wrap-around. C and D increment by 3-adic +1. For example, `...222 + 1 = ...000`: region 2 flows into region 0, as in Unshackled.
- Cells that were never written have the Malbolge fill value. The fill pattern repeats every 6 cells, is computed from the last two program cells, and is extended to all heads via the remainder rule `[R §1.2, §3.1]`. The VM materializes cells lazily (sparse pages).
- **Implementation limit:** n ≤ 39. Values are stored in a `u64` holding 40 trits, with trit 39 as the head. Exceeding n = 39 is a defined runtime error (`E_WIDTH`), not undefined behaviour.

### 2.3 Rotation width = n
- `rotr` rotates the low n trits (the value is head-extended to n trits first). Trits at positions ≥ n stay equal to the head.
- Because n is deterministic, compiled code knows the rotation width exactly. It can restore a value non-destructively by rotating it n times. No Unshackled-style bootstrap is needed.
- Generated programs fix n at start-up by moving D to a sufficiently wide address once (startup code, §5.1). After that they keep all addresses below that width.

### 2.4 I/O (raw bytes)
- **In:** A = the byte value 0..255. EOF → `...222` (−1).
- **Out:** head 0 → write `A mod 256`. Head 2 → write the (negative) integer mod 256. Head 1 → runtime error `E_OUTPUT`.
- No newline translation. stdin and stdout are binary.
- stdout is flushed before every `In` and on halt or error.

### 2.5 Loader and errors
- **Strict loader:** whitespace is skipped. Every other byte must be 33..126 and must decode to one of the 8 instructions at its position. The program must have at least 2 cells. There is no maximum length.
- **Pure programs only:** memory starts as exactly the loaded program followed by the fill pattern. A `.malm` file can never preload data. All data is built by code at runtime.
- Runtime errors replace the reference interpreter's undefined behaviour and hangs:
  - `E_EXEC`: executing a cell outside 33..126
  - `E_ENCRYPT`: encrypting such a cell
  - `E_WIDTH`
  - `E_OUTPUT`

  Each error exits the VM with status 1 and a diagnostic: C, D, A, n, step count.
- Halt (`v`) → exit 0. With the container flag `EXIT_A`, halt exits with `A mod 256` instead (§5.4).

### 2.6 `--classic` mode
- n is fixed at 10. Values are plain 10-trit numbers (no 3-adic head). C and D wrap at 59049.
- EOF = 59048. Output = A mod 256.
- Uses the same strict loader and the same defined errors.
- Purpose: conformance testing against the known Malbolge corpus `[R §6]`, and usefulness as a classic Malbolge interpreter.

## 3. Architecture

A single C++23 repository, built with CMake + Ninja, and one CLI binary `malm`.

```
source (C/C++/Rust/Zig/… or .py/.lua via RV32 interpreter build)
   │  real compiler (clang --target=riscv32, rustc, zig) + malm runtime
   ▼
RV32IM ELF ──┬─ emu mode: emu.malm + startup code that builds the RAM image ─┐
             └─ aot mode: rv-aot → HLA → asm ────────────────────────────────┤
                                                                             ▼
                                                                        prog.malm ── malm pack ──► native executable
                                                                             │
                                                   stdin ──► malm run / exe ─┴──► stdout
```

| Component | Directory | Responsibility | Depends on |
|---|---|---|---|
| `core` | `src/core` | values, crz/rot/xlat tables, remainder rule, sparse memory, loader, step/run loop, unleashed + classic modes | — |
| `container` | `src/container` | `.malm` read/write, normalization, zstd | core |
| `asm` | `src/asm` | low-level assembler modelled on HeLL/LMAO: `.CODE` cycle units, `.DATA` D-driven lists, labels, `R_` restores, auto immutable-Nops, flags (cycle length 2/4/5/6/9), layout, startup code for constants that can't be loaded directly, n-forcing → `.malm` | core, container |
| `disasm` | `src/asm` | `.malm`/Malbolge text → readable asm (for debugging) | core |
| `hla` | `src/hla` | high-level assembly: virtual registers, mov, branch, call/ret via `j`, indirect RAM load/store, 32-bit binary arithmetic library (table-based), I/O → asm | asm |
| `rv-emu` | `rv/emu` | RV32IM emulator written in HLA → `emu.malm`, built at toolchain build time | hla |
| `rv-aot` | `src/aot` | RV32IM ELF → HLA, one block per instruction, **reusing rv-emu's instruction-semantics macros** | hla, rv-emu macros |
| `runtime` | `rv/runtime` | RISC-V side: crt0, linker script, `ecall` syscalls (`read`, `write`, `exit`, `brk`), minimal libc; later picolibc and a Rust `no_std`+alloc runtime | — |
| `driver` | `src/driver` | `malm build`: choose the compiler by extension (`--cc`, `--` passthrough), link the runtime, choose the mode, produce `.malm` / exe | all |
| `pack` | `src/pack` | append a `.malm` + trailer to a copy of the `malm` binary; detect the trailer at startup | container |

### 3.1 CLI
- `malm run <file> [--classic] [--trace] [--stats]`: runs `.malm` or plain Malbolge text.
- `malm asm <file.mua> -o x.malm`
- `malm hla <file.mhl> -o x.malm`
- `malm disasm <file>`
- `malm build <src> [--mode emu|aot] [-o x.malm | --exe -o x] [--cc <compiler>] [-- <compiler args>]`
- `malm pack <x.malm> -o <exe>`
- `malm export --text <x.malm>`: writes plain Malbolge text.

## 4. `.malm` format and native executables

- **Container:** magic `MALM`, format version, mode (`unleashed` | `classic`), flags (`EXIT_A`), codec, cell count, CRC32, then the payload.
- **Payload:** the program in *normalized* form (8 symbols, 3 bits per cell, packed), compressed with zstd. zstd comes via CMake FetchContent and is statically linked. Denormalization on load reproduces the exact program text. The container holds the program and nothing else; it carries no data `[R §4.1, §5d]`.
- `malm run` also accepts plain Malbolge text (`.mal`, `.mu`, or any file without the `MALM` magic).
- **Native executable:** `malm pack` copies the running `malm` binary (`/proc/self/exe`; built statically) and appends the `.malm` file plus a trailer (`MALMEXE1`, payload offset, length). At startup, `malm` checks its own tail. If a trailer is present, it runs the attached program with the process's stdin and stdout and ignores the CLI. No C++ compiler is needed at pack time.
- **Exit status:** `A mod 256` on halt when `EXIT_A` is set; otherwise 0 on halt. 1 on a VM error.

## 5. Toolchain details

### 5.1 asm (low level)
- The source syntax follows HeLL `[R §4.3]`, so the published LMAO examples can be ported directly. The exact syntax is fixed in the asm ticket and documented in `docs/asm.md`.
- **Responsibilities:**
  - placement: put each instruction at an address where its xlat2 cycle (e.g. the FJ 2-cycle) decodes correctly
  - restore semantics
  - flags
  - immutable-Nop padding
  - **startup code generation**: straight-line code that builds every non-loadable `.DATA` value with crz/rot, because preloading data is not allowed (§2.5)
  - n-forcing at startup
- The `.DATA` area and computed addresses may be placed in any head region. Startup code accounts for the growth rule.

### 5.2 hla (high level)
- 32-bit binary words are stored in a representation decided by the hla arithmetic spike ticket. Candidates: 4 byte-cells, or 2 halves of 16 bits. Bitwise operations and add-with-carry use lookup tables (unbounded memory makes 256×256 tables affordable).
- Reading RAM indirectly: MovD to the address, then read and restore by rotating n times (§2.3), or by crz with a known value.
- Calls and returns use D-driven return addresses.

### 5.3 RISC-V
- **Target:** RV32IM, little-endian, no compressed instructions. Built with `clang --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32`.
- **emu mode:** `emu.malm` + the program's ELF segments, which are turned into startup code that writes guest RAM. The guest RAM size is fixed per build (`--ram`, default 1 MiB), so n is fixed after startup.
- **aot mode:** a static control-flow graph is recovered from the ELF. Indirect jumps go through a dispatch table that maps guest PCs to blocks. Self-modifying guest code is not supported in aot mode; the build fails with an error naming the reason.

### 5.4 Syscalls and stdin piping
- `ecall`: `a7` = number, using Linux RV32 numbers for `read` (63), `write` (64), `exit` (93), `brk` (214).
- `read(0, …)` executes malbolge `In` once per byte. EOF (`...222`) → return value 0.
- `write(1|2, …)` executes `Out` once per byte. stderr is merged into stdout in v1.
- `exit(code)`: the program loads `code & 255` into A and executes `Hlt`. If the container flag `EXIT_A` is set (always set by `malm build`), the VM uses `A mod 256` at halt as the process exit status. Without the flag, halt → 0. No in-band stdout markers, so binary output stays clean.
- **Pipeline:** `cat input | ./prog` and `cat input | malm run prog.malm` behave identically.

## 6. Testing

| Layer | Tests |
|---|---|
| core | unit tests (crz, rotr at several n, xlat tables, remainder rule, fill pattern, growth rule, errors); **classic conformance** on the ~17 corpus programs with expected SHA-256 outputs, including negative cases (`answer.mal`) `[R §6]`; **differential test** vs Olmstead's reference `malbolge.c` (built as a test fixture) on random valid programs |
| container/pack | round-trip normalize/denormalize/compress; pack → run equals run |
| asm | golden tests (hello, cat, halting cat, adder, digital root, as shipped with LMAO); purity check: every output passes the strict loader |
| hla | arithmetic: exhaustive at 8-bit, property-based at 32-bit vs C++ reference functions |
| rv-emu / rv-aot | official `riscv-tests` rv32ui/rv32um; **three-way differential**: emu vs aot vs a small C++ RV32 reference emulator (registers + stdout) |
| e2e | `malm build` for C, C++, Rust and Zig samples (hello, cat, wc, fizzbuzz, sort-stdin) with piped stdin, as `.malm` and as a packed exe |

- Framework: doctest. ctest labels: `fast` (run on every change, in CI) and `slow` (emu-mode tests; nightly CI).
- **Errors:** every diagnostic carries a source file:line (asm, hla, aot) or VM state (C, D, A, n, step). `--trace` prints one line per step; `--stats` prints counts per instruction, peak n, and touched pages.

## 7. Milestones

| Milestone | Scope | Exit criterion |
|---|---|---|
| **M0 Foundations** | repo/CMake/CI, language spec doc, core VM, classic conformance, `.malm` container, pack | classic corpus passes; `malm pack` exe runs a classic program with piped stdin |
| **M1 Assembler** | asm, disasm, ported LMAO examples | hello/cat/adder built by `malm asm` run in unleashed mode |
| **M2 HLA** | registers, control flow, calls, indirect RAM, 32-bit arithmetic library | arithmetic suite passes |
| **M3 RISC-V emu mode** | runtime/libc, rv-emu, riscv-tests, `malm build` for C, stdin end-to-end | `echo hi \| ./cat` built from `cat.c` echoes `hi` |
| **M4 AOT mode** | rv-aot, differential tests, size/speed | three-way differential passes on the e2e suite |
| **M5 More languages** | C++, Rust, Zig, picolibc; MicroPython/Lua (stretch) | e2e samples pass per language |
| **M6 Performance** | VM hot path, startup snapshot accelerator (identical to running startup code; checked by a test), compression tuning | documented benchmarks |

## 8. Risks

- **Execution speed in emu mode** (two interpreter layers). Mitigations: aot mode, the M6 VM work, and the startup snapshot.
- **Program size from startup code** (pure loading). Mitigation: zstd on normalized code. Malbolge20 reported about 200× growth from its init step `[R §3.2]`.
- **32-bit binary arithmetic on ternary memory** is the hardest design point in hla. It gets a spike ticket before the arithmetic library is implemented.
- **No published MalbolgeLISP toolchain** `[R §4.4]`. The asm/hla design follows HeLL/LMAO (open, GPLv3), which we re-implement rather than copy. No GPL code goes into the repo.
