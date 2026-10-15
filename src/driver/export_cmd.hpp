#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm export (--text | --malm) <in> -o <out> [--classic] [--exit-a] [--codec packed|zstd|zstd-packed]`.
// `args` excludes the subcommand name.
int export_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                   std::ostream& err);

} // namespace malm
