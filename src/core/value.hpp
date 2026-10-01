// 3-adic value type (spec section 1) and trit primitives.
//
// Classic mode decision: `--classic` uses a separate plain type, `ClassicWord` (a 10-trit
// unsigned integer 0..59048, classic.hpp), not this Value type. Value is only for unleashed mode.
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace malm::core {

inline constexpr int kTrits = 40;    // storage width; trit 39 is the repeating head
inline constexpr int kMaxWidth = 39; // largest representable width (spec 2.2)

namespace detail {
constexpr std::array<std::uint64_t, kTrits + 1> make_pow3() {
    std::array<std::uint64_t, kTrits + 1> t{};
    t[0] = 1;
    for (std::size_t i = 1; i < t.size(); ++i) {
        t[i] = t[i - 1] * 3;
    }
    return t;
}
} // namespace detail

// pow3[i] = 3^i for 0 <= i <= 40 (3^40 fits in u64).
inline constexpr std::array<std::uint64_t, kTrits + 1> pow3 = detail::make_pow3();

// kHalfPow3Minus1[w] = (3^w - 1) / 2, the value of w trits all equal to 1.
inline constexpr std::array<std::int64_t, kMaxWidth + 1> kHalfPow3Minus1 = [] {
    std::array<std::int64_t, kMaxWidth + 1> t{};
    for (std::size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<std::int64_t>((pow3[i] - 1) / 2);
    }
    return t;
}();

class Value {
  public:
    constexpr Value() noexcept = default;

    // Precondition: raw < 3^40. Every such raw is a valid value.
    static constexpr Value from_raw(std::uint64_t raw) noexcept {
        assert(raw < pow3[kTrits]);
        return Value(raw);
    }

    // Head 0 value with the trits of k; nullopt if its width would exceed 39.
    static constexpr std::optional<Value> natural(std::uint64_t k) noexcept {
        if (k >= pow3[kMaxWidth]) {
            return std::nullopt;
        }
        return Value(k);
    }

    // Negative k has head 2 (-1 = ...222); nullopt if the width would exceed 39.
    static constexpr std::optional<Value> from_int(std::int64_t k) noexcept {
        const auto lim = static_cast<std::int64_t>(pow3[kMaxWidth]);
        if (k >= lim || k < -lim) {
            return std::nullopt;
        }
        return Value(k >= 0 ? static_cast<std::uint64_t>(k)
                            : pow3[kTrits] - static_cast<std::uint64_t>(-k));
    }

    // Accepts `0t<trits>` and `...<head><low trits>` (spec 1.1); nullopt if malformed or wider
    // than 39 after canonicalisation.
    static std::optional<Value> parse(std::string_view text) noexcept;

    // Canonical notation: `0t<trits>` for head 0 (`0t0` for zero), `...<head><low trits>` else.
    std::string to_string() const;

    constexpr std::uint64_t raw() const noexcept { return raw_; }

    constexpr unsigned head() const noexcept {
        return static_cast<unsigned>(raw_ / pow3[kMaxWidth]);
    }

    // Trit i; i >= 39 yields the head.
    constexpr unsigned trit(unsigned i) const noexcept {
        if (i >= kMaxWidth) {
            return head();
        }
        return static_cast<unsigned>((raw_ / pow3[i]) % 3);
    }

    // True iff width() <= w (comparisons only; w >= 39 is always true).
    constexpr bool fits_width(unsigned w) const noexcept {
        if (w >= kMaxWidth) {
            return true;
        }
        const std::int64_t hh = head();
        const std::int64_t s = offset();
        const std::int64_t q = kHalfPow3Minus1[w];
        return -hh * q <= s && s <= (2 - hh) * q;
    }

    // Smallest w such that every trit at position w or above equals the head.
    constexpr unsigned width() const noexcept {
        unsigned lo = 0;
        unsigned hi = kMaxWidth;
        while (lo < hi) { // smallest w that fits; fits_width is monotone in w
            const unsigned mid = (lo + hi) / 2;
            if (fits_width(mid)) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        return lo;
    }

    // 3-adic v + 1. ...222 + 1 = 0 is fine; a carry into trit 39 that changes the head to
    // anything else would need width 40 and yields nullopt (E_WIDTH at the VM level).
    constexpr std::optional<Value> incremented() const noexcept {
        if (raw_ == pow3[kTrits] - 1) {
            return Value(0);
        }
        const std::uint64_t next = raw_ + 1;
        if (next / pow3[kMaxWidth] != raw_ / pow3[kMaxWidth]) {
            return std::nullopt;
        }
        return Value(next);
    }

    // Remainder rule (spec 1.4); m > 0. The result is non-negative.
    constexpr std::uint32_t mod(std::uint32_t m) const noexcept {
        assert(m > 0);
        const unsigned h = head();
        const std::int64_t s = offset();
        const std::int64_t r = h == 0 ? 0 : (h == 1 ? 29524 : 59048);
        const auto mm = static_cast<std::int64_t>(m);
        std::int64_t x = (r + s) % mm;
        if (x < 0) {
            x += mm;
        }
        return static_cast<std::uint32_t>(x);
    }
    constexpr std::uint32_t mod94() const noexcept { return mod(94); }
    constexpr std::uint32_t mod6() const noexcept { return mod(6); }

    // No ordering: 3-adic values have none at language level (spec 1.3). Use raw() for keys.
    friend constexpr bool operator==(const Value&, const Value&) noexcept = default;

  private:
    // raw - H, where H is the all-h 40-trit value (wrapping): exactly the sum over i < 39 of
    // (t_i - h) * 3^i, since its magnitude is below 3^39 < 2^63.
    constexpr std::int64_t offset() const noexcept {
        const std::uint64_t all_h = head() * ((pow3[kTrits] - 1) / 2);
        return static_cast<std::int64_t>(raw_ - all_h);
    }

    explicit constexpr Value(std::uint64_t raw) noexcept : raw_(raw) {}

    std::uint64_t raw_ = 0;
};

} // namespace malm::core

template <> struct std::hash<malm::core::Value> {
    std::size_t operator()(const malm::core::Value& v) const noexcept {
        return std::hash<std::uint64_t>{}(v.raw());
    }
};
