# malbolge-unleashed

malbolge-unleashed is a Malbolge variant with unbounded `3^n` memory, plus a toolchain around it:
assembler, high-level assembler, disassembler, a RISC-V front end (so C/C++/Rust/Zig programs and
Lua scripts can target Malbolge) and a packer that turns programs into native executables. Everything ships as one
CLI binary, `malm`. See the [design spec](docs/superpowers/specs/2026-10-01-malbolge-unleashed-design.md)
and the [Malbolge research notes](docs/research/malbolge-research.md).
The normative language reference is [docs/spec.md](docs/spec.md).
The assembly language (`.mua`) is specified in [docs/asm.md](docs/asm.md).
The high-level assembly language (`.mhl`) is specified in [docs/hla.md](docs/hla.md).
The command line (`run`, `asm`, `hla`, `export`, `pack`, `snapshot`, `--trace`, `--stats`) is documented in [docs/cli.md](docs/cli.md).

## Prerequisites

- C++23 compiler (GCC 14+ or Clang 18+), CMake 3.28+, Ninja
- For `malm build` of C and C++ programs and the RISC-V tests: clang with the riscv32 target and
  lld (`ld.lld`); without them those tests are skipped with a configure warning (`MALM_RV_TESTS`,
  [docs/rv.md](docs/rv.md)). With them, and `llvm-ar` or `ar`, the build also cross-compiles
  picolibc, libc++ and Lua into `malm` for `malm build --libc picolibc`, C++ and Lua scripts
  (`MALM_PICOLIBC`; it downloads LLVM's 140 MB source tarball once)
- For `malm build` of Rust programs and the Rust tests: rustc 1.85+ with the
  `riscv32im-unknown-none-elf` target (`rustup target add riscv32im-unknown-none-elf`); without it
  those tests are skipped with a configure warning (`MALM_RUST_TESTS`)
- For `malm build` of Zig programs and the Zig tests: Zig 0.17 ([ziglang.org](https://ziglang.org/download/));
  without it those tests are skipped with a configure warning (`MALM_ZIG_TESTS`)

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
| `src/container` | `.malm` format ([docs/malm-format.md](docs/malm-format.md)), startup snapshots ([docs/snapshot.md](docs/snapshot.md)) |
| `src/asm` | assembler and disassembler |
| `src/hla` | high-level assembler |
| `src/aot` | RV32IM ELF to HLA ahead-of-time translator |
| `src/driver` | `malm` CLI |
| `src/pack` | native executable packer |
| `src/rv` | RV32IM ELF loader, the C++ reference emulator (`malm rvref`) and the generator of rv-emu with its instruction semantics (`malm rvemu`, [docs/rv.md](docs/rv.md)) |
| `rv/emu` | builds `emu.malm`, the RV32IM emulator written in HLA |
| `rv/runtime` | RISC-V runtime: crt0, linker script, syscalls, minimal libc; picolibc and libc++ build and their system interface; the Rust runtime crate and the Zig runtime; Lua's interpreter ([docs/rv.md](docs/rv.md)) |
| `examples/asm` | assembly examples: hello world, cat, halting cat, adder, digital root |
| `examples/hla` | high-level assembly examples: hello world, cat, exit status, reverse (stack) |
| `examples/c` | C examples for the RISC-V runtime: hello world, cat, wc, fizzbuzz, sort; stats (floating point, picolibc) |
| `examples/cpp` | C++ examples (libc++): hello world, sort (`std::vector`, `std::sort`) |
| `examples/rust` | Rust examples (`no_std` with `alloc`): hello world, cat, sort |
| `examples/zig` | Zig examples: hello world, cat, sort |
| `examples/lua` | Lua scripts: hello world, cat, sort |
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

M3 (RISC-V emu mode): the RV32IM ELF loader and a C++ reference emulator, the test
oracle for emu and aot mode, which passes riscv-tests `rv32ui-p-*` and `rv32um-p-*`; run an ELF
with the hidden debugging command `malm rvref prog.elf` ([docs/rv.md](docs/rv.md)). The RISC-V
runtime (crt0, linker script, system calls and a minimal libc with malloc and printf) runs the C
programs in `examples/c` on the reference emulator ([docs/rv.md](docs/rv.md) section 6). rv-emu,
the RV32IM emulator written in HLA, is built into `emu.malm` with the toolchain, agrees with the
reference emulator instruction by instruction and passes riscv-tests `rv32ui-p-*` and `rv32um-p-*`
in emu mode. `malm build` compiles a C program with clang and the runtime (or takes an RV32IM ELF)
into a pure `.malm` that runs it in emu mode, or with `--exe` into a native executable
([docs/cli.md](docs/cli.md)): `malm build --ram 64K --exe -o cat examples/c/cat.c`, then
`echo hi | ./cat` prints `hi`, the M3 exit criterion.

M4 (aot mode): rv-aot translates an RV32IM program instruction by instruction into HLA, with
rv-emu's instruction semantics, a control-flow graph recovered from the ELF and a dispatch table
for indirect jumps ([docs/rv.md](docs/rv.md) section 9). `malm build --mode aot` builds C programs
and ELFs that way, about three times faster than emu mode, with programs about half the size of
the first version's ([docs/aot-benchmarks.md](docs/aot-benchmarks.md), issue #31). The three-way
differential test runs
every riscv-test `rv32ui-p-*` and `rv32um-p-*` and the e2e programs on the reference emulator, in
emu mode and in aot mode, and finds the same stdout, exit status and final registers (dumped by
ecall 255) on all three, which is the M4 exit criterion (`fence_i`, which modifies its own code,
is refused by aot mode).

M5 (more languages): `malm build --libc picolibc` links C programs with picolibc, the full C
library, and compiler-rt's soft-float routines, built into `malm` ([docs/rv.md](docs/rv.md)
section 6): `malm build --libc picolibc --ram 64K --exe -o stats examples/c/stats.c`, then
`echo 1.5 -2.25 1e3 | ./stats` prints the mean and standard deviation. C++ programs are built
with libc++ on picolibc, without exceptions and RTTI unless `-- -fexceptions` (emu mode only) or
`-- -frtti` turns them on: `malm build --exe -o sort examples/cpp/sort.cpp`, then
`printf 'b\na\n' | ./sort`. Rust programs are `no_std` crates with `alloc`, linked with malm's
runtime crate (the entry point, a heap on `brk`, stdin and stdout, `println!`, panics):
`malm build --ram 64K --exe -o cat examples/rust/cat.rs`, then `echo hi | ./cat`. Zig programs
(Zig 0.17) have a `pub fn main` and import `malm` for stdin, stdout and an allocator:
`malm build --ram 64K --exe -o cat examples/zig/cat.zig`. Lua scripts run in Lua 5.5's
interpreter, built into `malm` with picolibc, in emu mode only and slowly (a minute for
`print('hi')`): `malm build --ram 512K --exe -o hello examples/lua/hello.lua`.

M6 (performance, in progress): the VM's step loop runs programs in unleashed mode, emu and aot mode
included, 2.7 to 4 times as fast as at M3 (issue #37); `tests/bench/vm_bench.sh` is the benchmark
([docs/benchmarks.md](docs/benchmarks.md)). `malm snapshot prog.malm` saves the state after the
steps before the program's first I/O, its startup code, in `prog.malm.snap`, and `malm run` then
starts from it: identical to running those steps (`malm snapshot --check` compares the two), and
4.6 to 9.5 times as fast as them; a whole run of the 1 MiB emu-mode hello takes 3.0 s instead of
4.3 s, most of it now loading the program (issue #38, [docs/snapshot.md](docs/snapshot.md)).

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); note the Zstandard notice requirement when
distributing `malm` binaries or packed executables.
