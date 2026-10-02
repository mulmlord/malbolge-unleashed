#pragma once

// Per-position char <-> instruction mapping (research 1.2, spec 3.5, spec 4). A character is
// loadable at a position when its decode letter is one of the 8 instruction letters; for each
// (position mod 94, instruction) there is exactly one such character in 33..126.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/tables.hpp"

namespace malm::core {

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
        const char letter = letter_of_code(k);
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

// Symbol (index into kSymbolLetters) of an instruction. Op::Nop is the letter 'o'.
constexpr std::uint8_t symbol_of(Op op) noexcept {
    switch (op) {
    case Op::MovD:
        return 0;
    case Op::Jmp:
        return 1;
    case Op::Rot:
        return 2;
    case Op::Opr:
        return 3;
    case Op::Out:
        return 4;
    case Op::In:
        return 5;
    case Op::Hlt:
        return 6;
    case Op::Nop:
        return 7;
    }
    return 7;
}

// The unique character in 33..126 that is loadable at `pos` and decodes to `op` there.
constexpr std::uint8_t loadable_char(std::size_t pos, Op op) noexcept {
    return denormalize(pos, symbol_of(op));
}

} // namespace malm::core
