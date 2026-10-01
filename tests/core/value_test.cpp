#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include "core/value.hpp"

using malm::core::kMaxWidth;
using malm::core::kTrits;
using malm::core::pow3;
using malm::core::Value;

namespace {

Value V(const char* text) {
    auto v = Value::parse(text);
    REQUIRE(v.has_value());
    return *v;
}

// Naive reference for the remainder rule (spec 1.4): iterate the trits of the raw value.
std::uint32_t naive_mod(std::uint64_t raw, std::uint32_t m) {
    std::uint64_t t[kTrits];
    std::uint64_t x = raw;
    for (std::size_t i = 0; i < kTrits; ++i) {
        t[i] = x % 3;
        x /= 3;
    }
    const std::int64_t h = static_cast<std::int64_t>(t[kTrits - 1]);
    std::int64_t sum = h == 0 ? 0 : (h == 1 ? 29524 : 59048);
    for (std::size_t i = 0; i < kMaxWidth; ++i) {
        sum += (static_cast<std::int64_t>(t[i]) - h) * static_cast<std::int64_t>(pow3[i]);
    }
    const std::int64_t mm = m;
    return static_cast<std::uint32_t>(((sum % mm) + mm) % mm);
}

// Naive width: scan down from trit 38 using divisions.
unsigned naive_width(std::uint64_t raw) {
    const std::uint64_t h = raw / pow3[39];
    unsigned w = 39;
    while (w > 0 && (raw / pow3[w - 1]) % 3 == h) {
        --w;
    }
    return w;
}

constexpr int kCases = 120000;

// Random value with head h = 0..2 and width w = 0..39, both uniform. Writes them to *head, *width
// if given. The value is H + offset with offset uniform in [-h*q, (2-h)*q], q = (3^w - 1) / 2
// (that range is exactly the set of values of width <= w).
Value random_value(std::mt19937_64& rng, unsigned* head = nullptr, unsigned* width = nullptr) {
    const std::uint64_t h = rng() % 3;
    const std::uint64_t w = rng() % 40;
    const std::uint64_t q = (pow3[w] - 1) / 2;
    const std::uint64_t span = 2 * q + 1; // number of offsets
    const std::uint64_t pick = rng() % span;
    // offset = pick - h*q, computed in wrapping unsigned arithmetic.
    const std::uint64_t all_h = h * ((pow3[kTrits] - 1) / 2);
    if (head != nullptr) {
        *head = static_cast<unsigned>(h);
    }
    if (width != nullptr) {
        *width = static_cast<unsigned>(w);
    }
    return Value::from_raw(all_h + pick - h * q);
}

} // namespace

TEST_CASE("construction from naturals") {
    CHECK(Value::natural(0)->raw() == 0);
    CHECK(*Value::natural(64) == V("0t2101"));
    CHECK(Value::natural(pow3[39] - 1).has_value());
    CHECK_FALSE(Value::natural(pow3[39]).has_value());
    CHECK_FALSE(Value::natural(UINT64_MAX).has_value());
}

TEST_CASE("construction from signed integers") {
    CHECK(*Value::from_int(-1) == V("...222"));
    CHECK(*Value::from_int(-2) == V("...221"));
    CHECK(*Value::from_int(5) == *Value::natural(5));
    CHECK(Value::from_int(-1)->head() == 2);
    const auto lim = static_cast<std::int64_t>(pow3[39]);
    CHECK(Value::from_int(lim - 1).has_value());
    CHECK_FALSE(Value::from_int(lim).has_value());
    CHECK(Value::from_int(-lim).has_value());
    CHECK_FALSE(Value::from_int(-lim - 1).has_value());
    CHECK(Value::from_int(-lim)->raw() == 2 * pow3[39]);
    CHECK(Value::from_int(-lim)->width() == 39);
    CHECK_FALSE(Value::from_int(INT64_MIN).has_value());
    CHECK_FALSE(Value::from_int(INT64_MAX).has_value());
}

TEST_CASE("parse and to_string") {
    CHECK(V("0t0").raw() == 0);
    CHECK(V("0t2101").raw() == 64);
    CHECK(V("0t0002101").raw() == 64);
    CHECK(V("...0").raw() == 0);
    CHECK(V("...000").raw() == 0);
    CHECK(V("...222").raw() == pow3[40] - 1);
    CHECK(V("...1102") == V("...102"));
    CHECK(V("...1102") == V("...11102"));
    CHECK(V("...1102").width() == 2);

    CHECK(Value::from_raw(0).to_string() == "0t0");
    CHECK(Value::from_raw(64).to_string() == "0t2101");
    CHECK(V("...222").to_string() == "...2");
    CHECK(V("...111").to_string() == "...1");
    CHECK(V("...11102").to_string() == "...102");
    CHECK(V("...1022").to_string() == "...1022");
    CHECK(V("...0222").to_string() == "0t222");

    for (const char* s : {"0t0", "0t1", "0t2101", "...1", "...2", "...102", "...2101", "...1022"}) {
        CHECK(Value::parse(Value::parse(s)->to_string()) == Value::parse(s));
    }

    for (const char* bad : {"", "0t", "...", "0", "1", "0t3", "0t12x", "..1", "....1", "...3",
                            "...1x", "0T1", "0t-1", " 0t1", "0t1 ", "0t1...2", "...1 02"}) {
        CAPTURE(bad);
        CHECK_FALSE(Value::parse(bad).has_value());
    }
    // Padding with leading head digits is accepted and stripped.
    CHECK(*Value::parse("0t0" + std::string(39, '2')) ==
          *Value::parse("0t" + std::string(39, '2')));
    CHECK(Value::parse("0t0" + std::string(39, '2'))->width() == 39);
    CHECK(Value::parse("...22" + std::string(39, '1'))->width() == 39);
    CHECK(*Value::parse("...22" + std::string(39, '1')) ==
          *Value::parse("...2" + std::string(39, '1')));
    // Padded but still width 40 after stripping: rejected.
    CHECK_FALSE(Value::parse("0t00" + std::string(1, '1') + std::string(39, '0')).has_value());
    CHECK_FALSE(Value::parse("...22" + std::string(1, '1') + std::string(39, '2')).has_value());
    // Width limit: 39 low trits are fine, 40 are not.
    CHECK(Value::parse("0t" + std::string(39, '2')).has_value());
    CHECK_FALSE(Value::parse("0t1" + std::string(39, '0')).has_value());
    CHECK(Value::parse("...1" + std::string(39, '0')).has_value());
    CHECK_FALSE(Value::parse("...1" + std::string(40, '0')).has_value());
    CHECK(Value::parse("...1" + std::string(10, '1') + "02").has_value());
}

TEST_CASE("width, head, trit") {
    CHECK(V("...000").width() == 0);
    CHECK(V("...222").width() == 0);
    CHECK(V("...111").width() == 0);
    CHECK(V("...0001").width() == 1);
    CHECK(V("...1102").width() == 2);
    CHECK(V("0t2101").width() == 4);

    const Value w0 = V(("0t1" + std::string(38, '0')).c_str());
    const Value w1 = V(("...2" + std::string(39, '1')).c_str());
    const Value w2 = V(("...1" + std::string(39, '2')).c_str());
    CHECK(w0.width() == 39);
    CHECK(w1.width() == 39);
    CHECK(w2.width() == 39);
    CHECK(w0.head() == 0);
    CHECK(w1.head() == 2);
    CHECK(w2.head() == 1);
    CHECK(w1.trit(0) == 1);
    CHECK(w1.trit(38) == 1);
    CHECK(w1.trit(39) == 2);
    CHECK(w1.trit(40) == 2);
    CHECK(w1.trit(1000) == 2);
    CHECK(w2.trit(1000) == 1);

    const Value v = V("0t2101");
    CHECK(v.trit(0) == 1);
    CHECK(v.trit(1) == 0);
    CHECK(v.trit(2) == 1);
    CHECK(v.trit(3) == 2);
    CHECK(v.trit(4) == 0);
    CHECK(v.trit(1000) == 0);
}

TEST_CASE("increment") {
    CHECK(*V("...222").incremented() == V("0t0"));
    CHECK(*V("...0222").incremented() == V("0t1000"));
    CHECK(*V("...1022").incremented() == V("...100"));
    CHECK(*V("...1022").incremented() == V("...1100"));
    CHECK(*V("0t0").incremented() == V("0t1"));
    CHECK(*V("0t2").incremented() == V("0t10"));
    CHECK(*V("...221").incremented() == V("...222"));
    CHECK(*V("...2222").incremented() == V("0t0"));
    CHECK(*V("...111").incremented() == V("...112"));
    CHECK(*V("...112").incremented() == V("...1120"));
    CHECK(V("...112").incremented()->width() == 2);
    CHECK(*V("...1122").incremented() == V("...1200"));
    CHECK(*V("...12").incremented() == V("...120"));
    CHECK(*V("...122").incremented() == V("...1200"));

    // Overflow into trit 39 (width 40): heads 0 and 1.
    CHECK_FALSE(Value::from_raw(pow3[39] - 1).incremented().has_value());
    CHECK_FALSE(Value::from_raw(2 * pow3[39] - 1).incremented().has_value());
    // Largest width-39 values that do not overflow.
    CHECK(Value::from_raw(pow3[39] - 2).incremented().has_value());
    CHECK(Value::from_raw(2 * pow3[39] - 2).incremented().has_value());
    // Head 2: the only carry out of trit 39 is ...222 -> 0, which is not an overflow.
    CHECK(Value::from_raw(pow3[40] - 2).incremented()->raw() == pow3[40] - 1);
    CHECK(Value::from_raw(pow3[40] - 1).incremented()->raw() == 0);
    CHECK(Value::from_raw(2 * pow3[39]).incremented()->raw() == 2 * pow3[39] + 1);
    // Head 2 never overflows, whatever the low 39 trits are.
    std::mt19937_64 rng(7);
    for (int i = 0; i < 10000; ++i) {
        const std::uint64_t low = rng() % pow3[39];
        const auto raw = 2 * pow3[39] + low;
        CHECK(Value::from_raw(raw).incremented().has_value());
    }
}

TEST_CASE("remainder rule: worked examples") {
    // spec 9.3
    CHECK(V("...1102").mod94() == 6);
    CHECK(V("...1102").mod6() == 2);
    CHECK(V("...2101").mod94() == 0);
    CHECK(V("...2101").mod6() == 4);
    CHECK(V("...1").mod94() == 8);
    CHECK(V("...1").mod6() == 4);
    CHECK(V("...222").mod94() == 16);
    CHECK(V("...222").mod6() == 2);
    CHECK(V("...222").mod(256) == 168);
    CHECK(V("...111").mod94() == 29524 % 94);
    CHECK(V("...1022").mod6() == 5);
    CHECK(V("0t0").mod94() == 0);
    CHECK(V("0t2101").mod94() == 64);
    CHECK(V("0t2101").mod(256) == 64);
    CHECK(V("...1102").mod(1) == 0);
    CHECK(V("...222").mod(UINT32_MAX) == 59048);
    CHECK(V("...2101").mod(UINT32_MAX) == 59032);
}

TEST_CASE("random_value covers every head and width") {
    std::mt19937_64 rng(31337);
    bool seen[3][40] = {};
    for (int i = 0; i < kCases; ++i) {
        unsigned h = 0;
        unsigned w = 0;
        const Value v = random_value(rng, &h, &w);
        REQUIRE(v.head() == h);
        REQUIRE(v.width() <= w);
        seen[h][v.width()] = true;
    }
    for (unsigned h = 0; h < 3; ++h) {
        for (unsigned w = 0; w < 40; ++w) {
            CAPTURE(h);
            CAPTURE(w);
            CHECK(seen[h][w]);
        }
    }
}

TEST_CASE("property: width and fits_width match the trit-scanning definition") {
    std::mt19937_64 rng(4242);
    for (int i = 0; i < kCases; ++i) {
        const Value v = (i % 4 == 0) ? Value::from_raw(rng() % pow3[40]) : random_value(rng);
        const unsigned w = naive_width(v.raw());
        REQUIRE(v.width() == w);
        REQUIRE(v.fits_width(w));
        REQUIRE((w == 0 || !v.fits_width(w - 1)));
        REQUIRE(v.fits_width(39));
    }
}

TEST_CASE("property: head 0 mod equals integer mod") {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < kCases; ++i) {
        const std::uint64_t k = rng() % pow3[39];
        const Value v = Value::from_raw(k);
        REQUIRE(v.mod94() == k % 94);
        REQUIRE(v.mod6() == k % 6);
        REQUIRE(v.mod(256) == k % 256);
        REQUIRE(v.mod(7) == k % 7);
        REQUIRE(v.mod(65521) == k % 65521);
    }
}

TEST_CASE("property: mod matches naive trit iteration for all heads") {
    std::mt19937_64 rng(98765);
    for (int i = 0; i < kCases; ++i) {
        const Value v = (i % 4 == 0) ? Value::from_raw(rng() % pow3[40]) : random_value(rng);
        const std::uint64_t raw = v.raw();
        REQUIRE(v.mod94() == naive_mod(raw, 94));
        REQUIRE(v.mod6() == naive_mod(raw, 6));
        REQUIRE(v.mod(256) == naive_mod(raw, 256));
        REQUIRE(v.mod(3) == naive_mod(raw, 3));
    }
    // Short values (small widths) of every head.
    for (std::uint64_t h = 0; h < 3; ++h) {
        for (std::uint64_t low = 0; low < 5000; ++low) {
            const std::uint64_t raw = h * pow3[39] + low;
            REQUIRE(Value::from_raw(raw).mod94() == naive_mod(raw, 94));
        }
    }
}

TEST_CASE("property: increment is continuous under mod") {
    std::mt19937_64 rng(2024);
    for (int i = 0; i < kCases; ++i) {
        const Value v = (i % 4 == 0) ? Value::from_raw(rng() % pow3[40]) : random_value(rng);
        const auto inc = v.incremented();
        if (!inc || v.raw() == pow3[40] - 1) {
            continue; // overflow; ...222 -> 0 changes R_h, so continuity is not promised there
        }
        for (std::uint32_t m : {94u, 6u, 256u}) {
            REQUIRE(inc->mod(m) == (v.mod(m) + 1) % m);
        }
    }
    // Targeted: values near region boundaries.
    for (std::uint64_t h = 0; h < 3; ++h) {
        for (std::uint64_t k : {pow3[39] - 3, pow3[39] - 2, pow3[20] - 1, pow3[20],
                                std::uint64_t{0}, std::uint64_t{1}}) {
            const Value v = Value::from_raw(h * pow3[39] + k);
            const auto inc = v.incremented();
            if (inc) {
                CHECK(inc->mod94() == (v.mod94() + 1) % 94);
            }
        }
    }
}

TEST_CASE("property: to_string / parse round trip") {
    std::mt19937_64 rng(555);
    for (int i = 0; i < kCases; ++i) {
        const Value v = (i % 4 == 0) ? Value::from_raw(rng() % pow3[40]) : random_value(rng);
        const auto back = Value::parse(v.to_string());
        REQUIRE(back.has_value());
        REQUIRE(*back == v);
    }
}

TEST_CASE("hash") { CHECK(std::hash<Value>{}(V("...1102")) == std::hash<Value>{}(V("...11102"))); }
