#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm rvref [--ram <size>] [--trace] [--regs] [--] <prog.elf>` (hidden; docs/cli.md): runs an
// RV32IM ELF on the C++ reference emulator. `args` excludes the subcommand name.
int rvref_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

} // namespace malm
