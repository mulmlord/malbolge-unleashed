# Third-party notices

malbolge-unleashed itself is licensed under the MIT License (see `LICENSE`). It uses or ships the following
third-party material.

## Linked into every `malm` binary and every packed executable

| Component | Version | Licence | Notes |
|---|---|---|---|
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause (dual-licensed BSD / GPLv2; used under BSD) | `.malm` compression, statically linked. Full text: [`third_party/LICENSE-zstd.txt`](third_party/LICENSE-zstd.txt) |

**If you distribute `malm` binaries or executables built with `malm pack`**, the BSD licence requires you to
reproduce Zstandard's copyright notice, conditions and disclaimer in the documentation or other materials provided
with the distribution, e.g. by shipping `third_party/LICENSE-zstd.txt` alongside them.

## Built into `malm` when it is built with picolibc (`MALM_PICOLIBC`), and linked into guest programs built with `malm build --libc picolibc`, from C++ or from Lua

| Component | Version | Licence | Notes |
|---|---|---|---|
| [picolibc](https://github.com/picolibc/picolibc) | 1.8.12 | BSD-style (mostly BSD-2-Clause and BSD-3-Clause, per file) | the C library for RISC-V guests (`rv/runtime/CMakeLists.txt`), fetched at configure time. Full text: [`COPYING.picolibc`](https://github.com/picolibc/picolibc/blob/1.8.12/COPYING.picolibc) in its source |
| [compiler-rt](https://github.com/llvm/llvm-project/tree/main/compiler-rt) builtins | 20.1.8 | Apache-2.0 WITH LLVM-exception | soft-float and 64-bit integer routines for RISC-V guests, fetched at configure time. Full text: [`third_party/LICENSE-compiler-rt.txt`](third_party/LICENSE-compiler-rt.txt) |
| [libc++](https://libcxx.llvm.org/), libc++abi, libunwind | 20.1.8 | Apache-2.0 WITH LLVM-exception | the C++ standard library and runtime for RISC-V guests (libunwind only for exceptions), with the parts of LLVM libc that libc++ compiles in (floating-point `from_chars`); fetched at configure time with compiler-rt, the same LLVM release. Same licence text: [`third_party/LICENSE-compiler-rt.txt`](third_party/LICENSE-compiler-rt.txt) |
| [Lua](https://www.lua.org/) | 5.5.1 | MIT | the Lua interpreter for Lua scripts (`rv/runtime/lua`), fetched at configure time. Licence text: at the end of `lua.h` in its source, and [lua.org/license.html](https://www.lua.org/license.html) |

All of them are in `malm` as a library archive (about 2.0 MB, unpacked by `malm build`), and the
parts a program uses are linked into it. **If you distribute such a `malm` binary**, ship the
licence texts with it (the two above, and Lua's). **If you distribute a program built with `--libc
picolibc` or from C++** (a `.malm` or packed executable), picolibc's licences require its copyright
notices with it; the LLVM exception waives the conditions of compiler-rt, libc++, libc++abi and
libunwind for code embedded into a compiled program. **A program built from Lua** carries Lua's
interpreter too, whose MIT licence asks for Lua's copyright notice with it.

## Linked into guest programs built from Rust (from the user's Rust toolchain, not part of `malm`)

| Component | Licence | Notes |
|---|---|---|
| Rust's `core`, `alloc` and `compiler_builtins` | MIT OR Apache-2.0 | precompiled for `riscv32im-unknown-none-elf` by rustup; the parts a program uses are linked into it |

**If you distribute a program built from Rust** (a `.malm` or packed executable), the MIT licence
(or Apache-2.0's notice conditions, if you choose that one) asks for the Rust project's copyright
notice with it, as for any Rust binary.

## Linked into guest programs built from Zig (from the user's Zig installation, not part of `malm`)

| Component | Licence | Notes |
|---|---|---|
| Zig's standard library and compiler-rt | MIT | compiled with the program by zig; the parts a program uses are linked into it |

**If you distribute a program built from Zig**, the MIT licence asks for Zig's copyright notice
with it.

## Used only for building and testing (not part of any binary we ship)

| Component | Licence | Location |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) 2.5.3 | MIT | fetched at configure time (`cmake/MalmTests.cmake`) |
| [riscv-tests](https://github.com/riscv-software-src/riscv-tests) (commit 7b68f7b) | BSD-3-Clause | fetched at configure time (`tests/rv/CMakeLists.txt`) |
| Ben Olmstead's reference interpreter `malbolge.c` | public domain (stated in its header) | `tests/reference/olmstead/` |
| Malbolge20 sample `hello20.mb` (Nagoya Univ.) | MIT | `tests/corpus/`, licence text in `tests/corpus/LICENSE-malbolge20.txt` |
| Malbolge programs from the esolangs.org wiki | CC0 (Esolang:Copyrights) | `tests/programs/`, `tests/corpus/` |
| Output of Matthias Ernst's `stringout.c` | CC0 (the tool's dedication) | `tests/corpus/` |

Per-file provenance is recorded in `tests/programs/SOURCES.txt`, `tests/corpus/SOURCES.md` and
`tests/reference/olmstead/README.md`.
