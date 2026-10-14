// The RISC-V toolchain behind `malm build` for C (docs/rv.md 6): clang with the riscv32 target
// compiles the program, ld.lld links it with a runtime built into malm (runtime_files.hpp).
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

// The C library a program is linked with (docs/rv.md 6): the minimal runtime, whose sources are
// compiled with the program, or picolibc, prebuilt.
enum class Libc { Minimal, Picolibc };

// Compiles the C `sources` against `libc` and links an RV32IM ELF executable with the runtime's
// crt0 and linker script, in a temporary directory. `args` are the user's compiler arguments (`--`
// in `malm build`): `-Wl,a,b` passes `a` and `b` to the linker, everything else goes to the
// compiler for `sources` (after the default -O2, so it can override it). The tools' output
// (diagnostics, also warnings) is copied to `err`; the error is a one-line summary. Fails with a
// message naming the compiler if it cannot compile for riscv32, and for picolibc if malm was built
// without it.
core::Result<std::vector<std::uint8_t>, std::string>
compile_c(const Toolchain& tools, std::span<const std::string_view> sources,
          std::span<const std::string_view> args, std::ostream& err, Libc libc = Libc::Minimal);

} // namespace malm
