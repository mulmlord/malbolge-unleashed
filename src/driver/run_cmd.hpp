#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

#include "driver/program_io.hpp"

namespace malm {

// Runs an already loaded program on `in`/`out`; `exit_a` is OR-ed with the program's own flag.
// Returns the process exit status (halt: 0 or A mod 256 with EXIT_A; fault: 1 plus a diagnostic).
int run_loaded(const LoadedProgram& prog, bool exit_a, std::istream& in, std::ostream& out,
               std::ostream& err);

// `malm run <file> [--exit-a]`. `args` excludes the subcommand name.
int run_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err);

} // namespace malm
