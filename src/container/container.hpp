// The .malm container (docs/malm-format.md): normalized, 3-bit packed, optionally zstd-compressed
// program cells. Pure: it carries program cells only, never data (spec 4).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/placement.hpp"
#include "core/result.hpp"
#include "core/tables.hpp"

namespace malm::container {

enum class Mode : std::uint8_t { Unleashed = 0, Classic = 1 };
enum class Codec : std::uint8_t { Packed = 0, Zstd = 1 };

inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::string_view kMagic = "MALM";
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint8_t kFlagExitA = 1;

// The char <-> instruction-symbol mapping lives in core (placement.hpp) so the assembler can use it
// without the container library.
using core::denormalize;
using core::kNoSymbol;
using core::kSymbolLetters;
using core::normalize;

// Largest accepted cell count for the mode (same as the text loader).
constexpr std::size_t max_cells(Mode m) noexcept {
    return m == Mode::Classic ? static_cast<std::size_t>(core::kClassicCells)
                              : core::kUnleashedMaxCells;
}

struct Program {
    std::vector<std::uint8_t> cells; // each loadable at its position (33..126)
    Mode mode = Mode::Unleashed;
    bool exit_a = false;
};

// Reader policy (not a format rule): the largest cell count read_malm accepts, on top of the
// format limit of the mode. Checked against the header before anything is decompressed.
struct Limits {
    std::size_t max_cells = core::kDefaultMaxCells;
};

// CRC-32 (IEEE 802.3, reflected, 0xEDB88320).
std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept;

// Precondition: 2 <= cells.size() <= max_cells(mode) and every cell loadable at its position.
std::vector<std::uint8_t> write_malm(const Program& p, Codec codec = Codec::Zstd);

// Validates everything (docs/malm-format.md); the error is a one-line reason. zstd payloads are
// decompressed as a stream in 1 MiB chunks and the cells grow only as data is produced, so memory
// is bounded by the data actually present (and by limits.max_cells), never by the header's claim.
core::Result<Program, std::string> read_malm(std::span<const std::uint8_t> bytes,
                                             Limits limits = {});

// True if `bytes` starts with the magic. "MALM" is never a valid text program: 'M' is not loadable
// at cell 0.
bool is_malm(std::span<const std::uint8_t> bytes) noexcept;

} // namespace malm::container
