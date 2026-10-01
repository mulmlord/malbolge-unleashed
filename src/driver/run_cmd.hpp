#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm run <file> [--exit-a]`. `args` excludes the subcommand name.
int run_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err);

} // namespace malm
