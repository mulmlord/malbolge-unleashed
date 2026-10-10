#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm rvemu [--ram <size>] [--harness] [-o <out.mhl>] [--] [<prog.elf>]` (hidden; docs/cli.md):
// writes the HLA program of rv-emu, the RV32IM emulator (docs/rv.md 8), optionally with a guest
// program in its RAM. `args` excludes the subcommand name.
int rvemu_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

} // namespace malm
