#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm hla [--text] [--map <file>] -o <out> <file.mhl>` and `malm hla (--check | --print)
// <file.mhl>` (docs/cli.md). `args` excludes the subcommand name.
int hla_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err);

} // namespace malm
