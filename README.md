# malbolge-unleashed

malbolge-unleashed is a Malbolge variant with unbounded `3^n` memory, plus a toolchain around it:
assembler, high-level assembler, disassembler, a RISC-V front end (so C/C++/Rust/Zig programs can
target Malbolge) and a packer that turns programs into native executables. Everything ships as one
CLI binary, `malm`. See the [design spec](docs/superpowers/specs/2026-10-01-malbolge-unleashed-design.md)
and the [Malbolge research notes](docs/research/malbolge-research.md).
The normative language reference is [docs/spec.md](docs/spec.md).
The assembly language (`.mua`) is specified in [docs/asm.md](docs/asm.md).
The command line (`run`, `asm`, `export`, `pack`, `--trace`, `--stats`) is documented in [docs/cli.md](docs/cli.md).

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
| `examples/asm` | assembly examples: hello world, cat, halting cat, adder, digital root |
| `tests/` | doctest suites (labels `fast`, `slow`) |
| `docs/` | spec, CLI reference and research |

## Status

Milestone M0 (Foundations) is complete: `malm run` executes malbolge-unleashed and classic Malbolge
(`--classic`, checked step by step against Olmstead's reference interpreter), with `--trace`/`--stats`,
the compressed `.malm` format (`malm export`) and standalone native executables (`malm pack`).
M1 (the assembler): `malm asm` assembles `.mua` programs into pure `.malm` files; the examples in
`examples/asm` (`malm asm examples/asm/adder.mua -o adder.malm`, then `echo 123 456 | malm run
adder.malm`) run in unleashed mode, which is the M1 exit criterion. The disassembler (`malm disasm`)
is still to come; see [docs/asm.md](docs/asm.md) and the GitHub milestones.

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); note the Zstandard notice requirement when
distributing `malm` binaries or packed executables.
