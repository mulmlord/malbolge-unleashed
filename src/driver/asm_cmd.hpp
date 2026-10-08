#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm asm [--text] [--map <file>] -o <out> <file.mua>` and `malm asm (--check | --print)
// <file.mua>` (docs/cli.md). `args` excludes the subcommand name.
int asm_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err);

} // namespace malm
