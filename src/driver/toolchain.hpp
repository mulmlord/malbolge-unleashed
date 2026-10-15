// The RISC-V toolchains behind `malm build` (docs/rv.md 6): for C and C++ clang with the riscv32
// target compiles the program, ld.lld links it with a runtime built into malm (runtime_files.hpp);
// for Rust rustc compiles and links it with the runtime crate, also built into malm.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace malm {

struct Toolchain {
    std::string cc; // clang
    std::string ld; // ld.lld
};

// Finds the compiler and the linker. `cc` is `--cc`: a path if it has a '/', else a name searched
// in `path_env` (the PATH variable); empty means clang, or the newest clang-N, in PATH. ld.lld is
// looked for next to the compiler (after resolving symlinks, which finds Ubuntu's
// /usr/lib/llvm-N/bin), then as ld.lld or the newest ld.lld-N in PATH. The error says what is
// missing.
core::Result<Toolchain, std::string> find_toolchain(std::string_view cc, std::string_view path_env);

// A C source ends in .c, a C++ source in .cpp, .cc or .cxx, a Rust source in .rs.
bool is_c_source(std::string_view path);
bool is_cxx_source(std::string_view path);
bool is_rust_source(std::string_view path);

// Whether the compiler arguments turn C++ exceptions on: the last of -fexceptions and
// -fno-exceptions is -fexceptions.
bool wants_exceptions(std::span<const std::string_view> args);

// The C library a program is linked with (docs/rv.md 6): the minimal runtime, whose sources are
// compiled with the program, or picolibc, prebuilt. C++ needs picolibc, which comes with libc++.
enum class Libc { Minimal, Picolibc };

// Compiles the C and C++ `sources` against `libc` and links an RV32IM ELF executable with the
// runtime's crt0 and linker script, in a temporary directory, and with libc++ if there is a C++
// source. `args` are the user's compiler arguments (`--` in `malm build`): `-Wl,a,b` passes `a` and
// `b` to the linker, everything else goes to the compiler for `sources` (after the defaults, -O2
// and for C++ -fno-exceptions -fno-rtti, so it can override them; with -fexceptions, libc++ is the
// one built with exceptions). The tools' output (diagnostics, also warnings) is copied to `err`;
// the error is a one-line summary. Fails with a message naming the compiler if it cannot compile
// for riscv32, for picolibc if malm was built without it, and for C++ with the minimal C library.
core::Result<std::vector<std::uint8_t>, std::string>
compile_c(const Toolchain& tools, std::span<const std::string_view> sources,
          std::span<const std::string_view> args, std::ostream& err, Libc libc = Libc::Minimal);

// The Rust target malm builds for.
inline constexpr std::string_view kRustTarget = "riscv32im-unknown-none-elf";

// Finds the Rust compiler. `rustc` is `--rustc`: a path if it has a '/', else a name searched in
// `path_env`; empty means rustc in PATH. The error says what is missing.
core::Result<std::string, std::string> find_rustc(std::string_view rustc,
                                                  std::string_view path_env);

// Compiles the Rust crate whose root is `source` (its modules come from there, as rustc finds them)
// for kRustTarget and links an RV32IM ELF executable, in a temporary directory: first the runtime
// crate `malm` (rv/runtime/rust), then the program with `--extern malm` and the runtime's linker
// script, with rustc's own linker. `args` are the user's arguments (`--` in `malm build`):
// `-Wl,a,b` passes `a` and `b` to the linker (`-C link-arg`), everything else goes to rustc for
// `source`, after the defaults (`-C opt-level=2`, and `--edition 2024` unless `args` name an
// edition). The output, the error and the failures are as compile_c's; it also fails with a
// message if the target is not installed for `rustc`.
core::Result<std::vector<std::uint8_t>, std::string>
compile_rust(const std::string& rustc, std::string_view source,
             std::span<const std::string_view> args, std::ostream& err);

} // namespace malm
