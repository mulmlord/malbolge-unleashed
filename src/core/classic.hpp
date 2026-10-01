// Classic Malbolge (spec 8): 10-trit plain words, flat memory of 59049 cells, wrap-around.
// A separate word type, not Value with n = 10 (see the note at the top of value.hpp).
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "core/ops.hpp"
#include "core/tables.hpp"
#include "core/value.hpp"

namespace malm::core {

// A 10-trit unsigned integer 0..59048 (no head). Plain integer so mod and compare are the
// ordinary ones.
using ClassicWord = std::uint16_t;

inline constexpr std::uint32_t kClassicCells = 59049; // 3^10
inline constexpr ClassicWord kClassicEof = 59048;

// crz of two 10-trit words as two 5-trit chunks through the same table as crz (spec 8, 3.2).
constexpr ClassicWord classic_crz(ClassicWord a, ClassicWord d) noexcept {
    assert(a < kClassicCells && d < kClassicCells);
    const std::uint32_t lo = detail::kCrz5.v[(d % 243u) * 243u + a % 243u];
    const std::uint32_t hi = detail::kCrz5.v[(d / 243u) * 243u + a / 243u];
    return static_cast<ClassicWord>(lo + hi * 243u);
}

// Rotate the 10 trits right by one.
constexpr ClassicWord classic_rotr(ClassicWord v) noexcept {
    assert(v < kClassicCells);
    return static_cast<ClassicWord>(v / 3u + (v % 3u) * 19683u);
}

// C and D increment with wrap-around at 59049.
constexpr ClassicWord classic_inc(ClassicWord v) noexcept {
    return v == kClassicEof ? ClassicWord{0} : static_cast<ClassicWord>(v + 1u);
}

class ClassicMemory {
  public:
    // Preconditions (the loader enforces them): every cell in 33..126. The program length is
    // checked even in release builds: throws std::length_error unless 2 <= L <= 59049. The
    // remaining cells are mem[i] = crz(mem[i-1], mem[i-2]) (spec 8).
    explicit ClassicMemory(std::span<const std::uint8_t> program) : cells_(kClassicCells) {
        if (program.size() < 2 || program.size() > kClassicCells) {
            throw std::length_error("classic program must have 2..59049 cells");
        }
        std::copy(program.begin(), program.end(), cells_.begin());
        for (std::size_t i = program.size(); i < kClassicCells; ++i) {
            cells_[i] = classic_crz(cells_[i - 1], cells_[i - 2]);
        }
    }

    // Precondition: addr < 59049.
    ClassicWord read(ClassicWord addr) const noexcept {
        assert(addr < kClassicCells);
        return cells_[addr];
    }
    void write(ClassicWord addr, ClassicWord v) noexcept {
        assert(addr < kClassicCells);
        cells_[addr] = v;
    }
    std::span<const ClassicWord> cells() const noexcept { return cells_; }

  private:
    std::vector<ClassicWord> cells_;
};

// Machine policy of BasicVm for classic mode (see vm.hpp).
class ClassicMachine {
  public:
    using Word = ClassicWord;

    explicit ClassicMachine(std::span<const std::uint8_t> cells) : mem_(cells) {}

    const ClassicMemory& memory() const noexcept { return mem_; }
    Word read(Word a) const noexcept { return mem_.read(a); }
    void write(Word a, Word v) noexcept { mem_.write(a, v); }

    // Brings an externally supplied word into range (set_registers).
    static Word normalize(Word w) noexcept { return static_cast<Word>(w % kClassicCells); }
    static bool is_cell(Word w) noexcept { return w >= 33 && w <= 126; }
    // Precondition: is_cell(w).
    static std::uint32_t cell_code(Word w) noexcept { return w; }
    static Word encrypted(Word w) noexcept { return static_cast<Word>(encrypt(cell_code(w))); }
    static std::uint32_t mod94(Word w) noexcept { return w % 94u; }
    static std::uint32_t mod256(Word w) noexcept { return w % 256u; }
    static Word crz(Word a, Word d) noexcept { return classic_crz(a, d); }
    Word rotr(Word v) const noexcept { return classic_rotr(v); }
    static Word eof() noexcept { return kClassicEof; }
    static Word from_byte(std::uint8_t b) noexcept { return b; }
    static std::optional<Word> incremented(Word w) noexcept { return classic_inc(w); }
    bool grow(Word, Word) noexcept { return true; }
    unsigned n() const noexcept { return 10; }
    // Diagnostics show classic words as naturals.
    static Value to_value(Word w) noexcept { return *Value::natural(w); }

  private:
    ClassicMemory mem_;
};

} // namespace malm::core
