// The arithmetic sequences and tables of src/hla/word.hpp (issue #21), checked by simulating them
// on core values: every sequence against its C++ meaning, the whole add (column sums, nibble
// indices, table lookups, merges) against x + y and the compares, shifts against >> and <<.

#include <doctest/doctest.h>

#include <random>
#include <set>
#include <unordered_map>

#include "core/ops.hpp"
#include "hla/word.hpp"

using namespace malm;
using namespace malm::hla;
using core::crz;

namespace {

const Value kC1 = make_value({}, 1);

// Runs `s` on the cells (pointers may alias); every load must be loadable (hla::load). The checks
// in the hot paths here only fire on a mismatch, so the exhaustive cases stay cheap.
void simulate(const Sequence& s, std::initializer_list<Value*> cells) {
    std::optional<Value> a;
    for (const Step& st : s) {
        if (st.cell < 0) {
            if (st.k.head() == 2 || !st.k.fits_width(kWidth)) {
                FAIL_CHECK("a constant that cannot be loaded");
            }
            a = st.k;
        } else {
            REQUIRE(a.has_value());
            Value& c = *cells.begin()[st.cell];
            c = *a = crz(*a, c);
        }
    }
}

// A cell with the trits 0..kWidth - 1 random 0/2 and head 0.
Value random_f02(std::mt19937_64& rng) {
    std::vector<unsigned> low(kWidth);
    for (unsigned& t : low) {
        t = rng() % 2 ? 2 : 0;
    }
    return make_value(low, 0);
}

std::uint32_t sext8(std::uint32_t b) {
    return static_cast<std::uint32_t>(static_cast<std::int8_t>(b));
}

// The ALU of src/hla/lower.cpp on values: S := column sums, then per nibble the index, the table
// (digits back from the index), the result nibble merged into Z (C1 before) and the carry.
struct Alu {
    // index -> nibble, digits
    std::unordered_map<std::uint64_t, std::pair<unsigned, std::array<unsigned, 4>>> table;
    std::array<std::array<Sequence, 2>, kNibbles> masks; // [k][cin]

    Alu() {
        for (unsigned k = 0; k < kNibbles; ++k) {
            for (unsigned cin = 0; cin < 2; ++cin) {
                masks[k][cin] = mask_prefix(k);
                for (const Step& st : mask_last(k, cin)) {
                    masks[k][cin].push_back(st);
                }
            }
        }
        for (unsigned k = 0; k < kNibbles; ++k) {
            for (unsigned cin = 0; cin < 2; ++cin) {
                for (unsigned code = 0; code < 81; ++code) {
                    const std::array<unsigned, 4> d = {code % 3, code / 3 % 3, code / 9 % 3,
                                                       code / 27};
                    table[nibble_index(k, cin, d).raw()] = {k, d};
                }
            }
        }
    }

    // Returns the result and the carry-out.
    std::pair<std::uint32_t, unsigned> run(Value s, unsigned cin) {
        const Value s0 = s;
        Value z = kC1;
        for (unsigned k = 0; k < kNibbles; ++k) {
            Value j = kC1;
            simulate(masks[k][cin], {&s, &j});
            const auto it = table.find(j.raw());
            REQUIRE(it != table.end());
            if (it->second.first != k) {
                CHECK(it->second.first == k);
            }
            unsigned total = cin;
            for (unsigned b = 0; b < 4; ++b) {
                total += it->second.second[b] << b;
            }
            z = crz(word((total & 15) << (4 * k)), z);
            cin = total >> 4;
        }
        if (s != s0) {
            CHECK(s == s0); // swapped eight times
        }
        z = crz(kC1, z);
        const auto w = word_value(z);
        REQUIRE(w.has_value());
        return {*w, cin};
    }
};

Alu& alu() {
    static Alu a;
    return a;
}

// S := the column sums of x and y by `seq` (sum_sequence); X and Y must be kept.
Value sums(std::uint32_t x, std::uint32_t y, const Sequence& seq, bool same_cell = false) {
    Value cx = word(x);
    Value cy = word(y);
    Value s = kC1;
    simulate(seq, {&cx, same_cell ? &cx : &cy, &s});
    if (cx != word(x) || cy != word(y)) {
        CHECK(cx == word(x));
        CHECK(cy == word(y));
    }
    return s;
}

Digits all(Digit g) {
    Digits d;
    d.fill(g);
    return d;
}

const Sequence& add_sums() {
    static const Sequence s = sum_sequence(all(Digit::Add));
    return s;
}
const Sequence& sub_sums() {
    static const Sequence s = sum_sequence(all(Digit::Sub));
    return s;
}

Value sums_imm(std::uint32_t x, const Digits& d) {
    Value cx = word(x);
    Value s = kC1;
    simulate(sum_sequence_imm(d), {&cx, &s});
    CHECK(cx == word(x));
    return s;
}

// The digits of x + k (add), x + ~k (sltu, carry-in 1) and the signed variant (slt).
Digits imm_digits(std::uint32_t k, bool invert, bool signed_top) {
    Digits d;
    for (unsigned i = 0; i < kWordBits; ++i) {
        const bool e = ((invert ? ~k : k) >> i) & 1;
        if (signed_top && i == kWordBits - 1) {
            // 1 - x + k31: bit 31 of x ^ 0x80000000 plus that of ~(k ^ 0x80000000).
            d[i] = (k >> i) & 1 ? Digit::NotXPlus1 : Digit::NotX;
        } else {
            d[i] = e ? Digit::XPlus1 : Digit::X;
        }
    }
    return d;
}

// slt on registers: X's bit 31 flipped around the sums (Sub on bits 0..30, Add on bit 31).
Value slt_sums(std::uint32_t x, std::uint32_t y) {
    Value cx = word(x);
    Value cy = word(y);
    static const Sequence flip =
        transform(bit_map([](unsigned i) { return i == kWordBits - 1 ? Trit::Flip : Trit::Keep; }));
    static const Sequence seq = [] {
        Digits d = all(Digit::Sub);
        d[kWordBits - 1] = Digit::Add;
        return sum_sequence(d);
    }();
    simulate(flip, {&cx});
    if (word_value(cx) != (x ^ 0x80000000U)) {
        CHECK(word_value(cx) == (x ^ 0x80000000U));
    }
    Value s = kC1;
    simulate(seq, {&cx, &cy, &s});
    simulate(flip, {&cx});
    if (cx != word(x)) {
        CHECK(cx == word(x));
    }
    return s;
}

std::uint32_t lt(bool b) { return b ? 1 : 0; }

} // namespace

TEST_CASE("bitwise sequences: and, or, xor, also with X and Y the same cell") {
    std::mt19937 rng(21);
    const auto check = [&](const Sequence& s, auto op) {
        for (int i = 0; i < 300; ++i) {
            const auto x = static_cast<std::uint32_t>(rng());
            const auto y = i % 7 == 0 ? x : static_cast<std::uint32_t>(rng());
            Value cx = word(x);
            Value cy = word(y);
            Value cz = word(static_cast<std::uint32_t>(rng()));
            simulate(s, {&cx, &cy, &cz});
            CHECK(cx == word(x));
            CHECK(cy == word(y));
            CHECK(cz == word(op(x, y)));
            Value same = word(x);
            Value z2 = word(0xFFFFFFFF);
            simulate(s, {&same, &same, &z2});
            CHECK(same == word(x));
            CHECK(z2 == word(op(x, x)));
        }
    };
    check(and_sequence(), [](std::uint32_t a, std::uint32_t b) { return a & b; });
    check(or_sequence(), [](std::uint32_t a, std::uint32_t b) { return a | b; });
    check(xor_sequence(), [](std::uint32_t a, std::uint32_t b) { return a ^ b; });
}

TEST_CASE("column sums: x + y and x + 1 - y per bit, from registers and from constants") {
    std::mt19937 rng(21);
    for (int i = 0; i < 300; ++i) {
        const auto x = static_cast<std::uint32_t>(rng());
        const auto y = i % 5 == 0 ? x : static_cast<std::uint32_t>(rng());
        Digits d;
        for (Digit& g : d) {
            g = rng() % 2 ? Digit::Add : Digit::Sub;
        }
        const Value s = sums(x, y, sum_sequence(d), x == y && i % 2 == 0);
        Digits di;
        for (Digit& g : di) {
            g = std::array{Digit::X, Digit::XPlus1, Digit::NotX, Digit::NotXPlus1}[rng() % 4];
        }
        const Value si = sums_imm(x, di);
        for (unsigned t = 0; t <= kWidth; ++t) {
            const unsigned trit = t == kWidth ? s.head() : s.trit(t);
            const unsigned trit_i = t == kWidth ? si.head() : si.trit(t);
            if (!is_bit_trit(t)) {
                CHECK(trit == 1);
                CHECK(trit_i == 1);
                continue;
            }
            const unsigned b = t - kBit0;
            const unsigned xb = (x >> b) & 1;
            const unsigned yb = (y >> b) & 1;
            CHECK(trit == sigma(d[b] == Digit::Add ? xb + yb : xb + 1 - yb));
            const unsigned want = std::array<unsigned, 4>{
                xb, xb + 1, 1 - xb,
                2 - xb}[static_cast<std::size_t>(di[b]) - static_cast<std::size_t>(Digit::X)];
            CHECK(trit_i == sigma(want));
        }
    }
}

// The sum cell with its digit trits swapped 1 <-> 2, as an odd nibble's mask finds it.
Value swapped(Value s) {
    return trits([&](unsigned t) -> unsigned {
        const unsigned v = t == kWidth ? s.head() : s.trit(t);
        return is_bit_trit(t) ? std::array<unsigned, 3>{0, 2, 1}[v] : v;
    });
}

TEST_CASE("nibble indices: the mask builds them, they are distinct and lie in head 1") {
    std::set<std::uint64_t> seen;
    for (unsigned k = 0; k < kNibbles; ++k) {
        for (unsigned cin = 0; cin < 2; ++cin) {
            for (unsigned code = 0; code < 81; ++code) {
                const std::array<unsigned, 4> d = {code % 3, code / 3 % 3, code / 9 % 3, code / 27};
                const Value ix = nibble_index(k, cin, d);
                CHECK(ix.head() == 1);
                CHECK(ix.fits_width(kWidth - 1));
                CHECK(ix.trit(36) == 2);
                CHECK(seen.insert(ix.raw()).second);
                // S with these digits in nibble k (sigma-coded) and arbitrary digits elsewhere.
                Value s = trits([&](unsigned t) -> unsigned {
                    if (!is_bit_trit(t)) {
                        return 1;
                    }
                    const unsigned b = t - kBit0;
                    return b / 4 == k ? sigma(d[b % 4]) : (t * 7 + code) % 3;
                });
                if (k % 2 == 1) {
                    s = swapped(s);
                }
                const Value s0 = s;
                Value j = kC1;
                Sequence m = mask_prefix(k);
                for (const Step& st : mask_last(k, cin)) {
                    m.push_back(st);
                }
                simulate(m, {&s, &j});
                CHECK(s == swapped(s0));
                CHECK(j == ix);
            }
        }
    }
}

TEST_CASE("the ALU: add, sub, sltu and slt, exhaustive on sign-extended 8-bit operands") {
    // A debug build (sanitizers) takes every 7th pair; tests/hla/arith_test.cpp runs the VM.
#ifdef NDEBUG
    const std::uint32_t stride = 1;
#else
    const std::uint32_t stride = 7;
#endif
    Alu& a = alu();
    int wrong = 0;
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::uint32_t j = i % stride; j < 256; j += stride) {
            const std::uint32_t x = sext8(i);
            const std::uint32_t y = sext8(j);
            const auto sum = a.run(sums(x, y, add_sums()), 0).first;
            const auto [diff, carry] = a.run(sums(x, y, sub_sums()), 1);
            const auto signed_lt = a.run(slt_sums(x, y), 1).second == 0;
            if (sum != x + y || diff != x - y || (carry == 0) != (x < y) ||
                signed_lt != (static_cast<std::int32_t>(x) < static_cast<std::int32_t>(y))) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(sum == x + y);
                CHECK(diff == x - y);
                CHECK(lt(carry == 0) == lt(x < y));
                CHECK(signed_lt == (static_cast<std::int32_t>(x) < static_cast<std::int32_t>(y)));
                ++wrong;
            }
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("the ALU with a constant operand, on random words") {
    Alu& a = alu();
    std::mt19937 rng(21);
    for (int i = 0; i < 2000; ++i) {
        const auto x = static_cast<std::uint32_t>(rng());
        const auto k = i % 3 == 0 ? x ^ (1U << (rng() % 32)) : static_cast<std::uint32_t>(rng());
        CAPTURE(x);
        CAPTURE(k);
        CHECK(a.run(sums_imm(x, imm_digits(k, false, false)), 0).first == x + k);
        CHECK(a.run(sums_imm(x, imm_digits(k, true, false)), 1).second == lt(x >= k));
        CHECK(a.run(sums_imm(x, imm_digits(k, true, true)), 1).second ==
              lt(static_cast<std::int32_t>(x) >= static_cast<std::int32_t>(k)));
        const auto y = static_cast<std::uint32_t>(rng());
        CHECK(a.run(sums(x, y, add_sums()), 0).first == x + y);
        CHECK(a.run(slt_sums(x, y), 1).second ==
              lt(static_cast<std::int32_t>(x) >= static_cast<std::int32_t>(y)));
    }
}

TEST_CASE("transforms keep, clear, fill and flip trits, and keep the head 0") {
    std::mt19937_64 rng(21);
    for (int i = 0; i < 2000; ++i) {
        const Value h0 = random_f02(rng);
        TritMap m;
        const bool short_form = i % 2 == 0;
        for (Trit& t : m) {
            t = static_cast<Trit>(rng() % (short_form ? 2 : 4));
        }
        Value h = h0;
        const Sequence s = transform(m);
        simulate(s, {&h});
        CHECK(s.size() == (short_form ? 4U : 5U));
        CHECK(h.head() == 0);
        for (unsigned t = 0; t < kWidth; ++t) {
            const unsigned was = h0.trit(t);
            const unsigned want =
                std::array<unsigned, 4>{was, 0, 2, 2 - was}[static_cast<std::size_t>(m[t])];
            CHECK(h.trit(t) == want);
        }
    }
}

TEST_CASE("shifts: rotations and the shift map give << and >>; sra by flipping") {
    std::mt19937 rng(21);
    const Sequence flip = transform(bit_map([](unsigned) { return Trit::Flip; }));
    for (int i = 0; i < 200; ++i) {
        const auto x = static_cast<std::uint32_t>(rng());
        for (unsigned k = 1; k < kWordBits; ++k) {
            for (const bool left : {false, true}) {
                Value h = word(x);
                for (unsigned r = 0; r < rotations(left, k); ++r) {
                    h = core::rotr(h, kWidth);
                }
                simulate(transform(shift_map(left, k)), {&h});
                CHECK(word_value(h) == (left ? x << k : x >> k));
            }
            // sra of a negative x: ~((~x) >> k).
            Value h = word(x | 0x80000000U);
            simulate(flip, {&h});
            for (unsigned r = 0; r < k; ++r) {
                h = core::rotr(h, kWidth);
            }
            simulate(transform(shift_map(false, k)), {&h});
            simulate(flip, {&h});
            CHECK(word_value(h) ==
                  static_cast<std::uint32_t>(static_cast<std::int32_t>(x | 0x80000000U) >> k));
        }
    }
}

TEST_CASE("sign tables: the lane index sequence with sign_mask builds sign_index") {
    std::mt19937_64 rng(21);
    for (int i = 0; i < 300; ++i) {
        const auto w = static_cast<std::uint32_t>(rng());
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            Value x = word(w);
            Value ix = random_f02(rng);
            Value a = kC1;
            a = ix = crz(a, ix);
            a = ix = crz(a, ix);
            a = x = crz(a, x);
            a = ix = crz(a, ix);
            a = sign_mask(lane);
            a = ix = crz(a, ix);
            a = ix = crz(a, ix);
            CHECK(x == word(w));
            CHECK(ix == sign_index(lane, static_cast<std::uint8_t>(w >> (8 * lane))));
            CHECK(ix.head() == 1);
            CHECK(ix.fits_width(kWidth - 1));
        }
    }
}
