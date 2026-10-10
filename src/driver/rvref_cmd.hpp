#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>

namespace malm {

// `malm rvref [--ram <size>] [--trace] [--regs] [--] <prog.elf>` (hidden; docs/cli.md): runs an
// RV32IM ELF on the C++ reference emulator. `args` excludes the subcommand name.
int rvref_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err);

// A --ram value: decimal bytes with an optional K (KiB) or M (MiB) suffix, a multiple of
// rv::kRamAlign up to rv::kMaxRamSize; nullopt otherwise.
std::optional<std::uint32_t> parse_ram_size(std::string_view text);

} // namespace malm
