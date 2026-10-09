// The word representation and the constants of the lowering (src/hla/word.hpp), checked by
// simulating the crz sequences of docs/hla.md 4 on core values.

#include <doctest/doctest.h>

#include <random>
#include <set>

#include "core/ops.hpp"
#include "hla/word.hpp"

using namespace malm;
using namespace malm::hla;
using core::crz;

namespace {

const Value kC1 = make_value({}, 1);

// A := v by the load sequence, checking that every Opr leaves its constant unchanged.
Value run_load(const Load& l) {
    CHECK((l.start == Value{} || l.start == kC1));
    CHECK(l.oprs.size() <= 4);
    Value a = l.start;
    for (const Value& k : l.oprs) {
        CHECK(k.head() != 2);
        CHECK(k.fits_width(kWidth));
        a = crz(a, k);
        CHECK(a == k);
    }
    return a;
}

Value random_value(std::mt19937_64& rng) {
    std::vector<unsigned> low(kWidth);
    for (unsigned& t : low) {
        t = static_cast<unsigned>(rng() % 3);
    }
    return make_value(low, static_cast<unsigned>(rng() % 2));
}

} // namespace

TEST_CASE("words: bit i is trit 4 + i as 0 or 2, and back") {
    std::mt19937 rng(19);
    for (int i = 0; i < 1000; ++i) {
        const auto w = static_cast<std::uint32_t>(rng());
        const Value v = word(w);
        CHECK(word_value(v) == w);
        CHECK(v.fits_width(kWidth - 2));
        CHECK(crz(kC1, v) == v); // read into A without a change
    }
    CHECK(word(0) == Value{});
    CHECK(word(1) == *Value::natural(2 * 81));
    CHECK_FALSE(word_value(*Value::natural(1)).has_value());
    CHECK_FALSE(word_value(*Value::natural(81)).has_value());
    CHECK_FALSE(word_value(kC1).has_value());
}

TEST_CASE("load reaches every value of width <= 38 with head 0 or 1") {
    std::mt19937_64 rng(19);
    for (int i = 0; i < 2000; ++i) {
        const Value v = random_value(rng);
        CHECK(run_load(load(v)) == v);
    }
    for (unsigned b = 0; b < 256; ++b) {
        CHECK(run_load(load(*Value::natural(b))) == *Value::natural(b));
    }
    CHECK(load(Value{}).oprs.empty());
    CHECK(load(kC1).oprs.empty());
}

TEST_CASE("crz_operand") {
    std::mt19937 rng(19);
    for (int i = 0; i < 200; ++i) {
        const Value w = word(static_cast<std::uint32_t>(rng()));
        const auto a = crz_operand(kC1, w);
        REQUIRE(a.has_value());
        CHECK(crz(*a, kC1) == w);
    }
    CHECK_FALSE(crz_operand(Value{}, *Value::natural(2)).has_value());
}

TEST_CASE("the lane index sequence builds the index and keeps the register") {
    std::mt19937_64 rng(19);
    for (int i = 0; i < 500; ++i) {
        const auto w = static_cast<std::uint32_t>(rng());
        const Value x0 = word(w);
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            for (const unsigned tag : {0U, 8U, 15U}) {
                // A := C1; Opr I; Opr I; Opr X; Opr I; A := K; Opr I; Opr I, I arbitrary before.
                Value x = x0;
                Value ix = random_value(rng);
                Value a = kC1;
                a = ix = crz(a, ix);
                a = ix = crz(a, ix);
                a = x = crz(a, x);
                a = ix = crz(a, ix);
                a = lane_mask(lane, tag);
                a = ix = crz(a, ix);
                a = ix = crz(a, ix);
                CHECK(x == x0);
                const auto byte = static_cast<std::uint8_t>(w >> (8 * lane));
                CHECK(ix == lane_index(lane, tag, byte));
            }
        }
    }
}

TEST_CASE("lane indices: the tables of the lowering do not overlap, in the head-1 window") {
    // The byte table (lane 0, tag 0, entries up to 26 cells) and the zero tables (lane l, tag
    // 8 + 2 (l & 1) + 4 (l >> 1), 2 cells), as in src/hla/lower.cpp.
    std::set<std::uint64_t> cells;
    const auto table = [&](unsigned lane, unsigned tag, std::uint64_t span) {
        for (unsigned b = 0; b < 256; ++b) {
            const Value ix = lane_index(lane, tag, static_cast<std::uint8_t>(b));
            CHECK(ix.head() == 1);
            CHECK(ix.fits_width(kWidth - 1));
            for (std::uint64_t k = 1; k <= span; ++k) {
                CHECK(cells.insert(ix.raw() + k).second);
            }
        }
    };
    table(0, 0, 26);
    for (unsigned lane = 0; lane < kLanes; ++lane) {
        table(lane, 8 | (lane & 1) << 1 | (lane >> 1) << 2, 2);
    }
}

TEST_CASE("the input table: bytes on 729 consecutive head-1 cells, EOF in head 0") {
    std::set<std::uint64_t> seen;
    const std::uint64_t base = in_index(0).raw() / 729 * 729;
    for (int b = 0; b < 256; ++b) {
        const Value v = in_index(b);
        CHECK(v.head() == 1);
        CHECK(v.raw() / 729 * 729 == base);
        CHECK(seen.insert(v.raw()).second);
        CHECK(v.trit(35) == 2);
    }
    const Value eof = in_index(-1);
    CHECK(eof.head() == 0);
    CHECK(eof.fits_width(kWidth - 2));
    CHECK(in_seed().head() == 0);
}
