// Machine policy of BasicVm for unleashed mode (see vm.hpp): 40-trit values, sparse memory with
// the growing width n.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "core/memory.hpp"
#include "core/ops.hpp"
#include "core/tables.hpp"
#include "core/value.hpp"

namespace malm::core {

class UnleashedMachine {
  public:
    using Word = Value;

    explicit UnleashedMachine(std::span<const std::uint8_t> cells) : mem_(cells) {}

    const Memory& memory() const noexcept { return mem_; }
    Memory& memory() noexcept { return mem_; }
    Word read(Word a) const { return mem_.read(a); }
    void write(Word a, Word v) { mem_.write(a, v); }
    Word read(Word a, Port p) const { return mem_.read(a, p); }
    void write(Word a, Word v, Port p) { mem_.write(a, v, p); }

    // Every Value is valid; nothing to do (see ClassicMachine::normalize).
    static Word normalize(Word w) noexcept { return w; }
    static bool is_cell(Word w) noexcept { return w.raw() >= 33 && w.raw() <= 126; }
    // Precondition: is_cell(w).
    static std::uint32_t cell_code(Word w) noexcept { return static_cast<std::uint32_t>(w.raw()); }
    static Word encrypted(Word w) noexcept { return Value::from_raw(encrypt(cell_code(w))); }
    static std::uint32_t mod94(Word w) noexcept { return w.mod94(); }
    static std::uint32_t mod256(Word w) noexcept { return w.mod(256); }
    static Word crz(Word a, Word d) noexcept { return core::crz(a, d); }
    Word rotr(Word v) const noexcept { return core::rotr(v, mem_.n()); }
    static Word eof() noexcept { return Value::from_raw(pow3[kTrits] - 1); } // ...222
    static Word from_byte(std::uint8_t b) noexcept { return Value::from_raw(b); }
    static std::optional<Word> incremented(Word w) noexcept { return w.incremented(); }
    [[nodiscard]] bool grow(Word c, Word d) noexcept { return mem_.grow_for(c, d); }
    unsigned n() const noexcept { return mem_.n(); }
    static Value to_value(Word w) noexcept { return w; }

  private:
    Memory mem_;
};

} // namespace malm::core
