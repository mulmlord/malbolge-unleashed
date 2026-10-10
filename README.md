# malbolge-unleashed

malbolge-unleashed is a Malbolge variant with unbounded `3^n` memory, plus a toolchain around it:
assembler, high-level assembler, disassembler, a RISC-V front end (so C/C++/Rust/Zig programs can
target Malbolge) and a packer that turns programs into native executables. Everything ships as one
CLI binary, `malm`. See the [design spec](docs/superpowers/specs/2026-10-01-malbolge-unleashed-design.md)
and the [Malbolge research notes](docs/research/malbolge-research.md).
The normative language reference is [docs/spec.md](docs/spec.md).
The assembly language (`.mua`) is specified in [docs/asm.md](docs/asm.md).
The high-level assembly language (`.mhl`) is specified in [docs/hla.md](docs/hla.md).
The command line (`run`, `asm`, `hla`, `export`, `pack`, `--trace`, `--stats`) is documented in [docs/cli.md](docs/cli.md).

## Prerequisites

- C++23 compiler (GCC 14+ or Clang 18+), CMake 3.28+, Ninja
- For the RISC-V runtime and tests (and later `malm build`): clang with the riscv32 target and lld
  (`ld.lld`); without them those tests are skipped with a configure warning (`MALM_RV_TESTS`,
  [docs/rv.md](docs/rv.md))

Arch: `sudo pacman -S --needed cmake ninja clang lld`

Ubuntu 24.04: `sudo apt-get install cmake ninja-build g++-14 clang-18 lld-18`; its default `c++` is GCC 13, so pick the compiler explicitly, e.g. `CXX=g++-14 cmake --preset debug` (CI uses GCC 14 and Clang 18).

## Build and test

```sh
cmake --preset debug            # Debug + ASan/UBSan + -Werror
cmake --build --preset debug
ctest --preset debug -L fast

cmake --preset release          # also: release-static (static malm binary)
cmake --build --preset release
```

Binaries land in `build/<preset>/src/driver/malm`. CMake options: `MALM_WERROR`, `MALM_SANITIZE`,
`MALM_STATIC`, `MALM_BUILD_TESTS`.

## Layout

| Path | Contents |
|---|---|
| `src/core` | values, tables, sparse memory, loader, run loop |
| `src/container` | `.malm` format ([docs/malm-format.md](docs/malm-format.md)) |
| `src/asm` | assembler and disassembler |
| `src/hla` | high-level assembler |
| `src/aot` | RV32IM ELF to HLA ahead-of-time translator |
| `src/driver` | `malm` CLI |
| `src/pack` | native executable packer |
| `src/rv` | RV32IM ELF loader, the C++ reference emulator (`malm rvref`) and the generator of rv-emu with its instruction semantics (`malm rvemu`, [docs/rv.md](docs/rv.md)) |
| `rv/emu` | builds `emu.malm`, the RV32IM emulator written in HLA |
| `rv/runtime` | RISC-V runtime: crt0, linker script, syscalls, minimal libc ([docs/rv.md](docs/rv.md)) |
| `examples/asm` | assembly examples: hello world, cat, halting cat, adder, digital root |
| `examples/hla` | high-level assembly examples: hello world, cat, exit status, reverse (stack) |
| `examples/c` | C examples for the RISC-V runtime: hello world, cat, fizzbuzz |
| `tests/` | doctest suites (labels `fast`, `slow`) |
| `docs/` | spec, CLI reference, research and design decisions (`docs/adr`) |
| `prototypes/` | throwaway spike code behind the ADRs; not built or tested |

## Status

Milestone M0 (Foundations) is complete: `malm run` executes malbolge-unleashed and classic Malbolge
(`--classic`, checked step by step against Olmstead's reference interpreter), with `--trace`/`--stats`,
the compressed `.malm` format (`malm export`) and standalone native executables (`malm pack`).
M1 (the assembler): `malm asm` assembles `.mua` programs into pure `.malm` files; the examples in
`examples/asm` (`malm asm examples/asm/adder.mua -o adder.malm`, then `echo 123 456 | malm run
adder.malm`) run in unleashed mode, which is the M1 exit criterion. The disassembler (`malm disasm`)
turns an assembled program back into a `.mua` that re-assembles to the same program, with
labels from `malm asm --map` or recovered from the control flow; see [docs/asm.md](docs/asm.md),
[docs/cli.md](docs/cli.md) and the GitHub milestones.
M2 (HLA): `malm hla` compiles `.mhl` programs with 32-bit registers, moves, jumps, branches on
zero, byte I/O, `halt <code>`, calls and returns with a stack, `switch` jump tables, computed
jumps, RAM with `load`/`store` through address registers, and the RV32IM arithmetic (`add`, `sub`,
`slt`/`sltu`, `and`/`or`/`xor`, shifts, sign extension, `mul`/`mulh*`, `div`/`rem`; design and
step counts in [docs/adr/0002-hla-arithmetic.md](docs/adr/0002-hla-arithmetic.md) and
[docs/adr/0003-hla-muldiv.md](docs/adr/0003-hla-muldiv.md)) ([docs/hla.md](docs/hla.md));
`malm hla examples/hla/cat.mhl -o cat.malm`, then `echo hi | malm run cat.malm`. The arithmetic
suite (`tests/hla/arith_test.cpp`, with its slow cases) passes, which is the M2 exit criterion.

M3 (RISC-V emu mode), in progress: the RV32IM ELF loader and a C++ reference emulator, the test
oracle for emu and aot mode, which passes riscv-tests `rv32ui-p-*` and `rv32um-p-*`; run an ELF
with the hidden debugging command `malm rvref prog.elf` ([docs/rv.md](docs/rv.md)). The RISC-V
runtime (crt0, linker script, system calls and a minimal libc with malloc and printf) runs the C
programs in `examples/c` on the reference emulator; how to compile and link against it by hand is
in [docs/rv.md](docs/rv.md) section 6, until `malm build` does it. rv-emu, the RV32IM emulator
written in HLA, is built into `emu.malm` with the toolchain and agrees with the reference emulator
instruction by instruction; until the emu-mode image builder exists, `malm rvemu prog.elf -o
prog.mhl` writes it with a program in its RAM for `malm hla` ([docs/rv.md](docs/rv.md) section 8).

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); note the Zstandard notice requirement when
distributing `malm` binaries or packed executables.
