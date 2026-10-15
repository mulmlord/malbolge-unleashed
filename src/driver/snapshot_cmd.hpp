// Startup snapshots (docs/snapshot.md): `malm snapshot` writes and checks them, `malm run` uses
// them.
#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "container/snapshot.hpp"
#include "driver/program_io.hpp"

namespace malm {

// Where the snapshot of the program file `path` lives: `<path>.snap`.
std::string snapshot_path(std::string_view path);

// True if snapshot_path(path) names an existing regular file.
bool has_snapshot(std::string_view path);

// The snapshot of `prog`, loaded from `path` with its file hash, for `malm run`: nullopt if there
// is none. A snapshot that cannot be used (unreadable, invalid, made for another program file, a
// classic program) is ignored with a note on `err`: it can only make a run faster.
std::optional<container::Snapshot> find_snapshot(std::string_view path, const LoadedProgram& prog,
                                                 std::ostream& err);

// `malm snapshot [--check] [--max-steps N] [--max-cells N] <file>`. `args` excludes the subcommand
// name.
int snapshot_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                     std::ostream& err);

} // namespace malm
