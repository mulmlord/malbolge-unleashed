#pragma once

// xlat tables and the instruction decode table (spec 3.1, 3.5).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace malm::core {

// Copied byte-for-byte from the reference interpreter (spec 3.1). Index 0 is character 33.
inline constexpr std::string_view kXlat1 = "+b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA\"lI"
                                           ".v%{gJh4G\\-=O@5`_3i<?Z';FNQuY]szf$!BS/|t:Pn6^Ha";

inline constexpr std::string_view kXlat2 = "5z]&gqtyfr$(we4{WP)H-Zn,[%\\3dL+Q;>U!pJS72FhOA1C"
                                           "B6v^=I_0/8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G\"i@";

static_assert(kXlat1.size() == 94 && kXlat2.size() == 94);

enum class Op : std::uint8_t { Jmp, Out, In, Rot, MovD, Opr, Nop, Hlt };

namespace detail {

constexpr Op op_of_letter(char c) noexcept {
    switch (c) {
    case 'i':
        return Op::Jmp;
    case '<':
        return Op::Out;
    case '/':
        return Op::In;
    case '*':
        return Op::Rot;
    case 'j':
        return Op::MovD;
    case 'p':
        return Op::Opr;
    case 'o':
        return Op::Nop;
    case 'v':
        return Op::Hlt;
    default:
        return Op::Nop;
    }
}

constexpr bool is_instruction_letter(char c) noexcept {
    for (const char x : std::string_view("ji*p</vo")) {
        if (x == c) {
            return true;
        }
    }
    return false;
}

// Letter for code k: xlat1[(k - 33) mod 94] (spec 3.5); +61 == -33 (mod 94) avoids negatives.
constexpr char letter_of_code(std::size_t k) noexcept { return kXlat1[(k + 61) % 94]; }

} // namespace detail

// kOpOfCode[k] for k = (C + [C]) mod 94.
inline constexpr std::array<Op, 94> kOpOfCode = [] {
    std::array<Op, 94> t{};
    for (std::size_t k = 0; k < t.size(); ++k) {
        t[k] = detail::op_of_letter(detail::letter_of_code(k));
    }
    return t;
}();

// kLoadableCode[k]: the letter of code k is one of the 8 instructions "ji*p</vo".
inline constexpr std::array<bool, 94> kLoadableCode = [] {
    std::array<bool, 94> t{};
    for (std::size_t k = 0; k < t.size(); ++k) {
        t[k] = detail::is_instruction_letter(detail::letter_of_code(k));
    }
    return t;
}();

namespace detail {
constexpr bool decode_table_ok() {
    constexpr std::array<std::pair<std::size_t, Op>, 8> expect{{{4, Op::Jmp},
                                                                {5, Op::Out},
                                                                {23, Op::In},
                                                                {39, Op::Rot},
                                                                {40, Op::MovD},
                                                                {62, Op::Opr},
                                                                {68, Op::Nop},
                                                                {81, Op::Hlt}}};
    std::size_t loadable = 0;
    for (const bool b : kLoadableCode) {
        loadable += b ? 1 : 0;
    }
    if (loadable != 8) {
        return false;
    }
    for (const auto& [k, op] : expect) {
        if (!kLoadableCode[k] || kOpOfCode[k] != op) {
            return false;
        }
    }
    return true;
}
} // namespace detail

static_assert(detail::decode_table_ok(), "decode table differs from spec 3.5");

// kOpOfSum[s] = kOpOfCode[s % 94] for s = C mod 94 + [C] <= 93 + 126: decoding without a modulo.
inline constexpr std::array<Op, 94 + 127> kOpOfSum = [] {
    std::array<Op, 94 + 127> t{};
    for (std::size_t s = 0; s < t.size(); ++s) {
        t[s] = kOpOfCode[s % 94];
    }
    return t;
}();

// Precondition: c_mod94 < 94, cell in 33..126.
constexpr Op decode(std::uint32_t c_mod94, std::uint32_t cell) noexcept {
    assert(c_mod94 < 94 && cell >= 33 && cell <= 126);
    return kOpOfSum[c_mod94 + cell];
}

// Precondition: cell in 33..126. Returns the code of xlat2[cell - 33].
constexpr std::uint32_t encrypt(std::uint32_t cell) noexcept {
    assert(cell >= 33 && cell <= 126);
    return static_cast<std::uint32_t>(static_cast<unsigned char>(kXlat2[cell - 33]));
}

// Loader rule (spec 4): byte in 33..126 whose letter at this position is one of "ji*p</vo".
constexpr bool is_loadable(std::uint32_t pos_mod94, std::uint32_t byte) noexcept {
    assert(pos_mod94 < 94);
    return byte >= 33 && byte <= 126 && kLoadableCode[(pos_mod94 + byte) % 94];
}

} // namespace malm::core
