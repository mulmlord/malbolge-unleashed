# malbolge-unleashed

malbolge-unleashed is a Malbolge variant with unbounded `3^n` memory, plus a toolchain around it:
assembler, high-level assembler, disassembler, a RISC-V front end (so C/C++/Rust/Zig programs can
target Malbolge) and a packer that turns programs into native executables. Everything ships as one
CLI binary, `malm`. See the [design spec](docs/superpowers/specs/2026-10-01-malbolge-unleashed-design.md)
and the [Malbolge research notes](docs/research/malbolge-research.md).
The normative language reference is [docs/spec.md](docs/spec.md).
The assembly language (`.mua`) is specified in [docs/asm.md](docs/asm.md).

## Prerequisites

- C++23 compiler (GCC 14+ or Clang 18+), CMake 3.28+, Ninja
- Later, for `malm build`: clang with the riscv32 target and lld

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
| `rv/emu` | RV32IM emulator written in HLA |
| `rv/runtime` | RISC-V runtime (crt0, syscalls, libc) |
| `tests/` | doctest suites (labels `fast`, `slow`) |
| `docs/` | spec and research |

## Status

Early development: only the CLI skeleton exists. See the GitHub issues and milestones.
