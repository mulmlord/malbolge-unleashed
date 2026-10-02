// The trailer of a packed executable (docs/malm-format.md, "Packed executables"):
// [binary][payload = a .malm file][MALMEXE1, payload offset u64 LE, payload length u64 LE].
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/result.hpp"

namespace malm::pack {

inline constexpr std::string_view kTrailerMagic = "MALMEXE1";
inline constexpr std::size_t kTrailerSize = 24;

struct Trailer {
    std::uint64_t offset = 0; // of the payload from the start of the file
    std::uint64_t length = 0; // of the payload
};

std::array<std::uint8_t, kTrailerSize> make_trailer(Trailer t) noexcept;

// Parses the last bytes of a file of `file_size` bytes. `tail` is the file's final
// kTrailerSize bytes (a shorter span means a file too small to carry one).
//   - no trailer magic: ok(nullopt), an ordinary file
//   - magic present and the fields consistent (offset > 0, offset + length + 24 == file_size,
//     no overflow): ok(trailer)
//   - magic present but the fields inconsistent: err(reason)
core::Result<std::optional<Trailer>, std::string> parse_trailer(std::span<const std::uint8_t> tail,
                                                                std::uint64_t file_size);

} // namespace malm::pack
