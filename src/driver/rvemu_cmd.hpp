#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

#include "hla/parser.hpp"

namespace malm {

// rv-emu's guest RAM is at most this many bytes: HLA's RAM limit of kMaxRamWords words.
inline constexpr std::uint32_t kMaxEmuRam = hla::kMaxRamWords * 4;

// `malm rvemu [--ram <size>] [--harness] [-o <out.mhl>] [--] [<prog.elf>]` (hidden; docs/cli.md):
// writes the HLA program of rv-emu, the RV32IM emulator (docs/rv.md 8), optionally with a guest
// program in its RAM. `args` excludes the subcommand name.
int rvemu_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

} // namespace malm
