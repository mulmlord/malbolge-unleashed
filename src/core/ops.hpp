#pragma once

// crz and rotr on 40-trit head-extended values (spec 3.2, 3.4).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "core/value.hpp"

namespace malm::core {

namespace detail {

// Plain struct with a raw array: under constant evaluation std::array::operator[] is an extra call
// per access, which pushes the table build over clang's constexpr step limit at -O1 and above.
struct Crz5Table {
    std::uint8_t v[243 * 243];
};

// kCrz5.v[d * 243 + a] = crz of two 5-trit chunks (zero-extended inside the chunk), spec 3.2 table
// T. Built with the recurrence r(d, a) = (r(d/3, a/3) mod 81) * 3 + T[d%3][a%3], which keeps the
// constexpr step count low; (0, 0) is five trits of 1.
inline constexpr Crz5Table kCrz5 = [] {
    constexpr std::uint8_t T[3][3] = {{1, 0, 0}, {1, 0, 2}, {2, 2, 1}};
    Crz5Table t{};
    t.v[0] = 121; // 11111 in base 3
    for (unsigned d = 0; d < 243; ++d) {
        for (unsigned a = (d == 0 ? 1 : 0); a < 243; ++a) {
            const unsigned prev = t.v[(d / 3) * 243 + a / 3] % 81;
            t.v[d * 243 + a] = static_cast<std::uint8_t>(prev * 3 + T[d % 3][a % 3]);
        }
    }
    return t;
}();

static_assert(kCrz5.v[0] == 121);
static_assert(kCrz5.v[242 * 243 + 242] == 121);
static_assert(kCrz5.v[242] == 0);
static_assert(kCrz5.v[242 * 243] == 242);

// crz of two 20-trit halves (each < 3^20 < 2^32), four 5-trit chunks, 32-bit arithmetic.
constexpr std::uint32_t crz20(std::uint32_t x, std::uint32_t y) noexcept {
    std::uint32_t r = 0;
    std::uint32_t scale = 1;
    for (int i = 0; i < 4; ++i) {
        r += kCrz5.v[(y % 243) * 243 + x % 243] * scale;
        x /= 243;
        y /= 243;
        scale *= 243;
    }
    return r;
}

} // namespace detail

// Trit-wise crazy operation over all 40 trits; trit 39 is the head, so the head is consistent.
constexpr Value crz(Value a, Value d) noexcept {
    constexpr std::uint64_t kHalf = 3486784401; // 3^20
    const std::uint64_t x = a.raw();
    const std::uint64_t y = d.raw();
    const std::uint64_t lo =
        detail::crz20(static_cast<std::uint32_t>(x % kHalf), static_cast<std::uint32_t>(y % kHalf));
    const std::uint64_t hi =
        detail::crz20(static_cast<std::uint32_t>(x / kHalf), static_cast<std::uint32_t>(y / kHalf));
    return Value::from_raw(lo + hi * kHalf);
}

// Rotate the low n trits right by one; trits >= n stay the head. Preconditions (asserted): 10 <= n
// <= 39 and width(v) <= n (spec 2.2).
constexpr Value rotr(Value v, unsigned n) noexcept {
    assert(n >= 10 && n <= kMaxWidth && v.fits_width(n));
    const std::uint64_t raw = v.raw();
    const std::uint64_t low = raw % pow3[n];
    const std::uint64_t rotated = low / 3 + (low % 3) * pow3[n - 1];
    return Value::from_raw(raw - low + rotated);
}

} // namespace malm::core
