#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// `malm asm (--check | --print) <file.mua>`; plain assembly is not implemented yet (#14-#16).
// `args` excludes the subcommand name.
int asm_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err);

} // namespace malm
