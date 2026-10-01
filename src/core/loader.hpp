// Strict text loader (spec 4): pure, no preloading.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "core/value.hpp"

namespace malm::core {

struct LoadError {
    std::size_t offset; // 0-based byte offset in the input (skipped bytes count)
    std::string reason;
};

// Largest L whose initial n stays <= 39 (spec 4.3): 3^38.
inline constexpr std::size_t kUnleashedMaxCells = static_cast<std::size_t>(
    std::min<std::uint64_t>(pow3[38], std::numeric_limits<std::size_t>::max()));

using LoadResult = Result<std::vector<std::uint8_t>, LoadError>;

// Returns the cells 0..L-1 (each in 33..126) or E_LOAD. `max_cells` is the largest accepted L;
// classic mode (#7) passes 59049.
LoadResult load_text(std::span<const std::uint8_t> bytes,
                     std::size_t max_cells = kUnleashedMaxCells);

} // namespace malm::core
