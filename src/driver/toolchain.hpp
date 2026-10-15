// The RISC-V toolchains behind `malm build` (docs/rv.md 6): for C and C++ clang with the riscv32
// target compiles the program, ld.lld links it with a runtime built into malm (runtime_files.hpp);
// for Rust rustc compiles and links it with the runtime crate, for Zig zig with the Zig runtime,
// both also built into malm. A Lua script is linked by ld.lld with Lua's interpreter.
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

// A C source ends in .c, a C++ source in .cpp, .cc or .cxx, a Rust source in .rs, a Zig source
// in .zig, a Lua script in .lua.
bool is_c_source(std::string_view path);
bool is_cxx_source(std::string_view path);
bool is_rust_source(std::string_view path);
bool is_zig_source(std::string_view path);
bool is_lua_source(std::string_view path);

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

// Builds an RV32IM ELF executable that runs the Lua script `script`, in a temporary directory:
// clang assembles the script into an object (lua/script.S, with its file name as the chunk name),
// which ld.lld links with Lua's interpreter (liblua.a in the picolibc sysroot: Lua and
// rv/runtime/lua/main.c), picolibc, crt0 and the linker script. `args` are the user's arguments
// (`--` in `malm build`): `-Wl,a,b` passes `a` and `b` to the linker; anything else is an error, as
// nothing is compiled. The output, the error and the failures are as compile_c's; it also fails if
// malm was built without picolibc or the script cannot be read.
core::Result<std::vector<std::uint8_t>, std::string>
compile_lua(const Toolchain& tools, std::string_view script, std::span<const std::string_view> args,
            std::ostream& err);

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

// The Zig target and CPU malm builds for, and the Zig release whose standard library the Zig
// runtime is written against (major.minor).
inline constexpr std::string_view kZigTarget = "riscv32-freestanding-none";
inline constexpr std::string_view kZigCpu = "generic_rv32+m";
inline constexpr std::string_view kZigVersion = "0.17";

// Finds the Zig compiler as find_rustc finds rustc: `zig` is `--zig`, empty means zig in PATH.
core::Result<std::string, std::string> find_zig(std::string_view zig, std::string_view path_env);

// Compiles the Zig program whose root is `source` (the files it imports come from there) for
// kZigTarget and links an RV32IM ELF executable with `zig build-exe`, in a temporary directory.
// The Zig runtime (rv/runtime/zig) is the root module: start.zig imports `source` as the module
// `program` and calls its `main`; `program` can import the module `malm` (malm.zig). `args` are
// the user's arguments (`--` in `malm build`): they come after the defaults (`-O ReleaseSafe`) and
// before the modules, so they apply to all of them; `-Wl,--defsym=<symbol>=<value>` defines a
// symbol in the linker script (zig has no other way to pass linker arguments, so any other
// `-Wl,` is an error). The output, the error and the failures are as compile_c's; it also fails
// if `zig` is not Zig kZigVersion.
core::Result<std::vector<std::uint8_t>, std::string>
compile_zig(const std::string& zig, std::string_view source, std::span<const std::string_view> args,
            std::ostream& err);

} // namespace malm
