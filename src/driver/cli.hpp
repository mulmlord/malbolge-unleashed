#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace malm {

// Runs the malm CLI. `args` excludes argv[0]. Returns the process exit code.
int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

} // namespace malm
