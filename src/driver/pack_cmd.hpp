#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm pack [--classic] [--exit-a] [--max-cells N] -o <out> [--] <in>`. `args` excludes the
// subcommand name. Linux only: the base binary is /proc/self/exe.
int pack_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                 std::ostream& err);

// As pack_command, with the base binary (and the file whose overwriting is refused) given by
// `self_exe` instead of /proc/self/exe; for tests.
int pack_command_with(std::span<const std::string_view> args, std::ostream& err,
                      const char* self_exe);

} // namespace malm
