#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "rv/elf.hpp"

namespace malm {

// The emu-mode image builder (docs/rv.md 8): rv-emu with `image` in its guest RAM of `ram_size`
// bytes, compiled to a pure .malm with EXIT_A. The startup code writes the image's segments into
// guest RAM and sets pc to its entry, sp to ram_size and the program break to its end, then the
// emulator starts. Preconditions: `ram_size` is a multiple of rv::kRamAlign up to kMaxEmuRam, and
// `image` comes from rv::load_elf with the same size. The error is an internal one (the generated
// program does not compile, or fails the purity check).
core::Result<std::vector<std::uint8_t>, std::string> build_emu(const rv::Image& image,
                                                               std::uint32_t ram_size);

// `malm build [--mode emu] [--ram <size>] [--cc <compiler>] (-o <out.malm> | --exe -o <out>)
// (<prog.c>... | <prog.elf>) [-- <compiler args>...]` (docs/cli.md): compiles C sources with the
// runtime (toolchain.hpp), or takes an RV32IM ELF, and builds an emu-mode .malm or a native
// executable of it. `args` excludes the subcommand name.
int build_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

// As build_command, with the base binary of --exe (as pack_command_with) and the PATH searched for
// the toolchain given; for tests.
int build_command_with(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
                       const char* self_exe, std::string_view path_env);

} // namespace malm
