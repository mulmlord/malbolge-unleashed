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
#include "core/result.hpp"
#include "core/tables.hpp"

namespace malm::container {

enum class Mode : std::uint8_t { Unleashed = 0, Classic = 1 };
enum class Codec : std::uint8_t { Packed = 0, Zstd = 1 };

inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::string_view kMagic = "MALM";
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint8_t kFlagExitA = 1;

// The 8 instruction letters in symbol order: symbol s is kSymbolLetters[s].
inline constexpr std::string_view kSymbolLetters = "ji*p</vo";
inline constexpr std::uint8_t kNoSymbol = 0xFF;

namespace detail {

struct SymbolTables {
    std::array<std::uint8_t, 94> sym_of_code{}; // by k = (pos + cell) mod 94; kNoSymbol if none
    std::array<std::uint8_t, 8> code_of_sym{};  // inverse
};

constexpr SymbolTables make_symbol_tables() {
    SymbolTables t;
    t.sym_of_code.fill(kNoSymbol);
    for (std::size_t k = 0; k < 94; ++k) {
        // Letter of code k is xlat1[(k - 33) mod 94] (spec 3.5).
        const char letter = core::kXlat1[(k + 61) % 94];
        for (std::size_t s = 0; s < 8; ++s) {
            if (kSymbolLetters[s] == letter) {
                t.sym_of_code[k] = static_cast<std::uint8_t>(s);
                t.code_of_sym[s] = static_cast<std::uint8_t>(k);
            }
        }
    }
    return t;
}

inline constexpr SymbolTables kTables = make_symbol_tables();

} // namespace detail

// Cell code 33..126 at position `pos` -> symbol 0..7 (its instruction letter), or kNoSymbol if the
// cell is not loadable there.
constexpr std::uint8_t normalize(std::size_t pos, std::uint8_t cell) noexcept {
    if (cell < 33 || cell > 126) {
        return kNoSymbol;
    }
    return detail::kTables.sym_of_code[(pos % 94 + cell) % 94];
}

// Inverse of normalize (research 1.2). Precondition: sym < 8.
constexpr std::uint8_t denormalize(std::size_t pos, std::uint8_t sym) noexcept {
    const std::size_t k = detail::kTables.code_of_sym[sym & 7u];
    std::size_t ch = (k + 94 - pos % 94) % 94;
    if (ch < 33) {
        ch += 94;
    }
    return static_cast<std::uint8_t>(ch);
}

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
