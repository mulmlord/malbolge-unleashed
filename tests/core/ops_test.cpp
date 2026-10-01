#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>

#include "core/ops.hpp"

using malm::core::crz;
using malm::core::kMaxWidth;
using malm::core::kTrits;
using malm::core::pow3;
using malm::core::rotr;
using malm::core::Value;

namespace {

Value V(const char* text) {
    auto v = Value::parse(text);
    REQUIRE(v.has_value());
    return *v;
}

Value natural(std::uint64_t k) { return *Value::natural(k); }

// Random value with head 0..2 and width 0..w_max, both uniform (see value_test.cpp).
Value random_value(std::mt19937_64& rng, unsigned w_max = 39) {
    const std::uint64_t h = rng() % 3;
    const std::uint64_t w = rng() % (w_max + 1);
    const std::uint64_t q = (pow3[w] - 1) / 2;
    const std::uint64_t pick = rng() % (2 * q + 1);
    const std::uint64_t all_h = h * ((pow3[kTrits] - 1) / 2);
    return Value::from_raw(all_h + pick - h * q);
}

Value random_raw(std::mt19937_64& rng) { return Value::from_raw(rng() % pow3[kTrits]); }

constexpr unsigned kT[3][3] = {{1, 0, 0}, {1, 0, 2}, {2, 2, 1}}; // spec 3.2, [d][a]

Value naive_crz(Value a, Value d) {
    std::uint64_t r = 0;
    for (unsigned i = 0; i < kTrits; ++i) {
        r += kT[d.trit(i)][a.trit(i)] * pow3[i];
    }
    return Value::from_raw(r);
}

// Spec 3.4 literally: r_i = t_{i+1} (i < n-1), r_{n-1} = t_0, r_i = head for i >= n.
Value naive_rotr(Value v, unsigned n) {
    std::uint64_t r = 0;
    for (unsigned i = 0; i < kTrits; ++i) {
        unsigned t = 0;
        if (i < n - 1) {
            t = v.trit(i + 1);
        } else if (i == n - 1) {
            t = v.trit(0);
        } else {
            t = v.head();
        }
        r += t * pow3[i];
    }
    return Value::from_raw(r);
}

// Reference interpreter op() on 10-trit words, spec 3.2 di-trit table.
std::uint32_t classic_op(std::uint32_t a, std::uint32_t d) {
    static constexpr unsigned o[9][9] = {
        {4, 3, 3, 1, 0, 0, 1, 0, 0}, {4, 3, 5, 1, 0, 2, 1, 0, 2}, {5, 5, 4, 2, 2, 1, 2, 2, 1},
        {4, 3, 3, 1, 0, 0, 7, 6, 6}, {4, 3, 5, 1, 0, 2, 7, 6, 8}, {5, 5, 4, 2, 2, 1, 8, 8, 7},
        {7, 6, 6, 7, 6, 6, 4, 3, 3}, {7, 6, 8, 7, 6, 8, 4, 3, 5}, {8, 8, 7, 8, 8, 7, 5, 5, 4}};
    std::uint32_t r = 0;
    for (const std::uint32_t p9 : {1u, 9u, 81u, 729u, 6561u}) {
        r += o[d / p9 % 9][a / p9 % 9] * p9;
    }
    return r;
}

} // namespace

TEST_CASE("crz equals the naive trit-by-trit reference on 1M pairs") {
    std::mt19937_64 rng(4242);
    for (int i = 0; i < 1'000'000; ++i) {
        const bool uniform = (i & 1) != 0;
        const Value a = uniform ? random_raw(rng) : random_value(rng);
        const Value d = uniform ? random_raw(rng) : random_value(rng);
        if (crz(a, d) != naive_crz(a, d)) {
            FAIL("crz mismatch at a=" << a.raw() << " d=" << d.raw());
        }
    }
}

TEST_CASE("crz basics") {
    CHECK(crz(natural(0), natural(0)) == V("...1"));
    CHECK(crz(V("...2"), V("...2")) == V("...1")); // T[2][2] = 1 in every position
    CHECK(crz(V("...1"), V("...0")) == V("...0"));
}

TEST_CASE("crz does not widen") {
    std::mt19937_64 rng(99);
    for (int i = 0; i < 200'000; ++i) {
        const Value a = random_value(rng);
        const Value d = random_value(rng);
        const unsigned w = crz(a, d).width();
        if (w > std::max(a.width(), d.width())) {
            FAIL("widened: a=" << a.raw() << " d=" << d.raw());
        }
    }
}

TEST_CASE("crz matches the classic op() on 10-trit naturals") {
    std::mt19937_64 rng(2024);
    for (int i = 0; i < 100'000; ++i) {
        const auto a = static_cast<std::uint32_t>(rng() % 59049);
        const auto d = static_cast<std::uint32_t>(rng() % 59049);
        const Value r = crz(natural(a), natural(d));
        CHECK_MESSAGE(r.raw() % 59049 == classic_op(a, d), "a=" << a << " d=" << d);
        CHECK(r.head() == 1);
    }
}

TEST_CASE("rotr at n = 10 matches the classic rotate on all 59049 values") {
    for (std::uint64_t v = 0; v < 59049; ++v) {
        if (rotr(natural(v), 10).raw() != v / 3 + (v % 3) * 19683) {
            FAIL("rotr mismatch at " << v);
        }
    }
}

TEST_CASE("rotr applied n times is the identity") {
    std::mt19937_64 rng(555);
    for (const unsigned n : {10u, 11u, 19u, 39u}) {
        for (int i = 0; i < 20'000; ++i) {
            const Value v = random_value(rng, n);
            Value r = v;
            for (unsigned k = 0; k < n; ++k) {
                r = rotr(r, n);
            }
            if (r != v) {
                FAIL("n=" << n << " v=" << v.raw());
            }
        }
    }
}

TEST_CASE("rotr equals the naive spec 3.4 reference for every n") {
    std::mt19937_64 rng(777);
    for (unsigned n = 10; n <= 39; ++n) {
        for (int i = 0; i < 20'000; ++i) {
            const Value v = random_value(rng, n); // precondition: width(v) <= n (spec 2.2)
            if (rotr(v, n) != naive_rotr(v, n)) {
                FAIL("n=" << n << " v=" << v.raw());
            }
        }
    }
}

TEST_CASE("rotr fixed points and head preservation") {
    for (const char* s : {"...000", "...111", "...222"}) {
        for (unsigned n = 10; n <= 39; ++n) {
            CHECK(rotr(V(s), n) == V(s));
        }
    }
    std::mt19937_64 rng(1);
    for (int i = 0; i < 10'000; ++i) {
        const Value v = random_value(rng);
        CHECK(rotr(v, 39).head() == v.head());
    }
}

TEST_CASE("rotr examples from spec 9.1 and 9.2") {
    CHECK(rotr(natural(75), 10) == natural(25));
    CHECK(rotr(V("...12202"), 10) == V("...12111111220"));
    CHECK(rotr(V("0t1"), 10).width() == 10);
}
