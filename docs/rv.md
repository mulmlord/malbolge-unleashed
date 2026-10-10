# RISC-V guests

The contract between a RISC-V guest program and the machines that run it: the C++ reference
emulator (`src/rv/machine.hpp`, `malm rvref`), rv-emu (`rv/emu`, emu mode) and rv-aot
(`src/aot`). The reference emulator is the oracle; the other two must agree with it on stdout,
exit status and, for the differential tests, the final registers. Design background:
[the design spec](superpowers/specs/2026-10-01-malbolge-unleashed-design.md) sections 5.3, 5.4
and 6.

## 1. Programs

A guest is an RV32IM ELF executable: little-endian ELF32, `ET_EXEC`, `EM_RISCV`, built with
`clang --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32`. The loader (`src/rv/elf.hpp`,
shared by all three machines) rejects, with a one-line reason:

- anything else (not ELF, 64-bit, big-endian, not `ET_EXEC`, another machine);
- the `e_flags` bits for compressed instructions (`EF_RISCV_RVC`), a hardware-float ABI or RV32E;
- program headers or segment data past the end of the file, `p_filesz > p_memsz`;
- `PT_LOAD` segments outside guest RAM or overlapping each other, or none at all;
- an entry point that is not 4-aligned inside an executable (`PF_X`) segment.

Only `PT_LOAD` segments with `p_memsz > 0` are loaded, at `p_vaddr`; `p_paddr`, section headers
and other program headers are ignored. Bytes past `p_filesz` are zero.

## 2. Machine state

- **Guest RAM** is the byte range `[0, ram_size)` and the whole address space. `ram_size` is fixed
  per program (`--ram`, default 1 MiB = `1M`): a multiple of 16 up to 1 GiB. RAM is zero except
  for the loaded segments. Code and data share it, without permissions: stores into code are
  seen by later fetches.
- **Registers** at the start: `pc` = the ELF entry, `sp` (x2) = `ram_size`, all others 0.
- **Program break** (`brk`) starts at the end of the highest segment (`p_vaddr + p_memsz`).

## 3. Instructions

RV32I and the M extension, as in the unprivileged ISA specification (division by zero and signed
overflow give the defined results). In addition:

- `fence` and `fence.i` are no-ops (one hart; every fetch reads RAM).
- Loads and stores may be misaligned; they access the bytes little-endian as usual.
- `ecall` is a system call (section 4).

Everything else faults (section 5): any encoding outside RV32IM (including compressed
instructions, CSR instructions, `ebreak`, reserved `funct3`/`funct7` values and `slli`/`srli`/`srai`
with `imm[5]` set), a taken jump or branch to an address that is not 4-aligned, a load, store or
fetch that is not entirely inside guest RAM.

## 4. System calls

`ecall` takes the number in `a7` and arguments in `a0`..`a2`, returns in `a0`, and changes no
other register. The numbers are those of Linux RV32. Errors are negated Linux errno values.

| `a7` | call | behaviour |
|---|---|---|
| 63 | `read(fd, buf, count)` | `fd` must be 0. Reads stdin one byte at a time into `buf` until `count` bytes or end of input; returns the number read, 0 at end of input. It never returns early with fewer bytes while input remains, so the result does not depend on how the host delivers input (rv-emu runs one Malbolge `In` per byte). Output is flushed first. |
| 64 | `write(fd, buf, count)` | `fd` must be 1 or 2; both go to stdout. Writes the bytes, returns `count`. |
| 93 | `exit(code)` | ends the program with exit status `code & 255`. |
| 214 | `brk(addr)` | if `initial break <= addr <= ram_size`, the break becomes `addr`; returns the break after the call (so `brk(0)` reads it). |

`read` from another fd and `write` to another fd return `-EBADF` (-9); a buffer that is not
entirely inside guest RAM gives `-EFAULT` (-14), with no I/O. Any other number returns `-ENOSYS`
(-38).

## 5. Faults

A fault stops the program with exit status 1 and a diagnostic on stderr; `pc` still names the
faulting instruction. Host I/O errors (stdout closed, a broken pipe, a read error) are faults
too, reported as `E_IO`. A guest that loops forever runs forever.

The reference emulator reports `malm rvref: <reason> at pc=0x... (step S)`, where `S` counts
instructions from 1, the faulting one included. Reasons:

- `illegal instruction 0x...` (the instruction word)
- `ebreak`
- `misaligned jump target 0x...`
- `load from 0x... outside guest RAM`, `store to 0x... outside guest RAM`
- `instruction fetch outside guest RAM`
- `E_IO: cannot write to stdout`, `E_IO: cannot read from stdin`

## 6. Tests

`tests/rv/`: unit tests for the loader and every instruction class (`rv_test`), the official
[riscv-tests](https://github.com/riscv-software-src/riscv-tests) `rv32ui-p-*` and `rv32um-p-*`
on `malm rvref`, and freestanding C programs (`tests/rv/programs/`).

riscv-tests is fetched at configure time (pinned commit) and built with clang and ld.lld, without
its `env` submodule: `tests/rv/env/riscv_test.h` replaces the machine-mode environment with one
that needs no CSRs or trap handler. A test starts at `_start` and exits through `ecall` 93 with
status 0 on pass and the failing test number (mod 256, never 0) on fail. Every test ELF is linked
with `tests/rv/link.ld` (one image from address `0x1000`).

The CMake cache variable `MALM_RV_TESTS` controls these toolchain-built tests: `AUTO` (default)
builds them when clang with the riscv32 target and ld.lld are found and otherwise skips them with
a warning, `ON` makes a missing toolchain a configure error (CI), `OFF` skips them. The
toolchain is found as `MALM_RV_CC` and `MALM_RV_LD`, which can also be set by hand.
