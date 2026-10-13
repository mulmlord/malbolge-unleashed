#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm rvaot [--ram <size>] [-o <out.mhl>] [--] <prog.elf>` (hidden; docs/cli.md): writes the HLA
// program that rv-aot translates the RV32IM program into (docs/rv.md 9). `args` excludes the
// subcommand name.
int rvaot_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

} // namespace malm
