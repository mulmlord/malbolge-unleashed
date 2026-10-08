#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm disasm [--map <file>] [--raw] [--classic] [--max-cells N] [-o <out>] <file>`
// (docs/cli.md). `args` excludes the subcommand name.
int disasm_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                   std::ostream& err);

} // namespace malm
