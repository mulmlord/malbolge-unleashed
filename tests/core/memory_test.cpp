#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "core/memory.hpp"
#include "core/ops.hpp"

using malm::core::crz;
using malm::core::kTrits;
using malm::core::Memory;
using malm::core::pow3;
using malm::core::Value;

namespace {

constexpr std::uint64_t kPage = 6561; // 3^8, must match Memory's page size

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

std::vector<std::uint8_t> random_program(std::mt19937_64& rng, std::size_t len) {
    std::vector<std::uint8_t> p(len);
    for (auto& c : p) {
        c = static_cast<std::uint8_t>(33 + rng() % 94);
    }
    return p;
}

// Spec 2.4 literally: f(L-2), f(L-1) are cells, f(i) = crz(f(i-1), f(i-2)).
std::vector<Value> recurrence(const std::vector<std::uint8_t>& prog, std::size_t upto) {
    const std::size_t len = prog.size();
    std::vector<Value> f(std::max(upto + 1, len));
    for (std::size_t i = 0; i < len; ++i) {
        f[i] = natural(prog[i]);
    }
    for (std::size_t i = len; i <= upto; ++i) {
        f[i] = crz(f[i - 1], f[i - 2]);
    }
    return f;
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

// Expected content of a never-written cell.
Value expected_fill(const Memory& m, Value addr) { return m.fill_pattern()[addr.mod6()]; }

// Raw addresses of the page containing `raw`: first and last cell.
std::uint64_t page_base(std::uint64_t raw) { return raw / kPage * kPage; }

} // namespace

TEST_CASE("spec 9.5: fill of the 2-cell program jv") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    const auto& p = m.fill_pattern();
    CHECK(p[0] == V("...1"));
    CHECK(p[1] == natural(80));
    CHECK(p[2] == V("...12222"));
    CHECK(p[3] == natural(40));
    CHECK(p[4] == V("...12222"));
    CHECK(p[5] == natural(80));

    // Head-1 address ...1022 has mod 6 = 5.
    const Value x = V("...1022");
    CHECK(x.mod6() == 5);
    CHECK(m.read(x) == natural(80));
    // Head-0 address beyond the program.
    CHECK(m.read(natural(1000)) == p[4]);
    CHECK(m.read(natural(1000)) == V("...12222"));
    // Program cells.
    CHECK(m.read(natural(0)) == natural(40));
    CHECK(m.read(natural(1)) == natural(80));
}

TEST_CASE("fill equals the crz recurrence for every head-0 address in [L, L+1000]") {
    std::mt19937_64 rng(101);
    for (int iter = 0; iter < 300; ++iter) {
        const std::size_t len = 2 + rng() % 299;
        const auto prog = random_program(rng, len);
        const auto f = recurrence(prog, len + 1000);
        Memory m(prog);
        for (std::size_t x = len; x <= len + 1000; ++x) {
            if (m.read(natural(x)) != f[x]) {
                FAIL("len=" << len << " x=" << x);
            }
        }
    }
}

TEST_CASE("low 10 trits of the fill equal the classic recurrence m[i] = op(m[i-1], m[i-2])") {
    auto check = [](const std::vector<std::uint8_t>& prog) {
        const std::size_t len = prog.size();
        std::vector<std::uint32_t> c(59049);
        for (std::size_t i = 0; i < len; ++i) {
            c[i] = prog[i];
        }
        for (std::size_t i = len; i < 59049; ++i) {
            c[i] = classic_op(c[i - 1], c[i - 2]);
        }
        Memory m(prog);
        for (std::size_t x = len; x < 59049; ++x) {
            if (m.read(natural(x)).raw() % 59049 != c[x]) {
                FAIL("len=" << len << " x=" << x);
            }
        }
    };
    std::mt19937_64 rng(202);
    for (int iter = 0; iter < 40; ++iter) {
        check(random_program(rng, 2 + rng() % 500));
    }
    for (const std::size_t len : {1000u, 6561u, 6562u, 19999u}) {
        check(random_program(rng, len));
    }
}

TEST_CASE("never-written cells of every head and width read as P[addr mod 6]") {
    std::mt19937_64 rng(303);
    const auto prog = random_program(rng, 50);
    Memory m(prog);
    const auto pages_before = m.page_count();

    std::vector<Value> addrs;
    for (const std::uint64_t raw :
         {pow3[39] - 1, pow3[39], 2 * pow3[39] - 1, 2 * pow3[39], pow3[40] - 1,
          std::uint64_t{0} + 50, kPage - 1, kPage, kPage + 1, 2 * kPage - 1, pow3[39] + kPage - 1,
          pow3[39] + kPage, 2 * pow3[39] + kPage, pow3[40] - kPage, pow3[40] - kPage - 1}) {
        addrs.push_back(Value::from_raw(raw));
    }
    for (int i = 0; i < 20000; ++i) {
        addrs.push_back(random_value(rng));
    }
    for (const Value a : addrs) {
        if (a.head() == 0 && a.raw() < 50) {
            continue; // program cell
        }
        if (m.read(a) != expected_fill(m, a)) {
            FAIL("addr raw=" << a.raw());
        }
    }
    CHECK(m.page_count() == pages_before); // reads never allocate

    // Address 0 is a program cell.
    CHECK(m.read(natural(0)) == natural(prog[0]));
}

TEST_CASE("materialising a page keeps the fill of its neighbouring cells (cyclic advance)") {
    std::mt19937_64 rng(404);
    const std::size_t len = 50;
    const auto prog = random_program(rng, len);
    Memory m(prog);

    std::vector<Value> addrs;
    for (const std::uint64_t raw :
         {pow3[39] - 1, pow3[39], 2 * pow3[39] - 1, 2 * pow3[39], pow3[40] - 1, std::uint64_t{0},
          kPage - 1, kPage, pow3[39] + 5, pow3[39] - kPage, 2 * pow3[39] - kPage}) {
        addrs.push_back(Value::from_raw(raw));
    }
    for (int i = 0; i < 100; ++i) {
        addrs.push_back(random_value(rng));
    }
    std::vector<std::uint64_t> written; // cells overwritten by earlier iterations
    for (const Value a : addrs) {
        const std::uint64_t base = page_base(a.raw());
        const Value marker = natural(7);
        m.write(a, marker);
        written.push_back(a.raw());
        CHECK(m.read(a) == marker);
        for (std::uint64_t k = 0; k < kPage; ++k) {
            const Value nb = Value::from_raw(base + k);
            if (nb == a) {
                continue;
            }
            Value want =
                (nb.head() == 0 && nb.raw() < len) ? natural(prog[nb.raw()]) : expected_fill(m, nb);
            if (std::find(written.begin(), written.end(), nb.raw()) != written.end()) {
                want = marker;
            }
            if (m.read(nb) != want) {
                FAIL("page of raw=" << a.raw() << " offset " << k);
            }
        }
    }
}

TEST_CASE("write and read round trip") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    const Value a = natural(1234);
    const Value before_left = m.read(natural(1233));
    const Value before_right = m.read(natural(1235));
    m.write(a, V("...1021"));
    CHECK(m.read(a) == V("...1021"));
    CHECK(m.read(natural(1233)) == before_left);
    CHECK(m.read(natural(1235)) == before_right);
    m.write(a, natural(5));
    CHECK(m.read(a) == natural(5));

    // High-head address.
    const Value h = V("...2210");
    m.write(h, V("...0121"));
    CHECK(m.read(h) == V("...0121"));
}

TEST_CASE("program cells can be overwritten") {
    const std::vector<std::uint8_t> prog = {40, 80, 33, 126};
    Memory m(prog);
    CHECK(m.read(natural(2)) == natural(33));
    m.write(natural(2), V("...2"));
    CHECK(m.read(natural(2)) == V("...2"));
    CHECK(m.read(natural(0)) == natural(40));
    CHECK(m.read(natural(3)) == natural(126));
}

TEST_CASE("a written value equal to the fill is kept") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    const Value x = natural(100000); // 100000 mod 6 = 4; page 15, not the program page
    const std::size_t pages = m.page_count();
    m.write(x, m.fill_pattern()[4]);
    CHECK(m.page_count() == pages + 1); // the write materialised a page
    CHECK(m.read(x) == m.fill_pattern()[4]);
    CHECK(m.read(natural(100001)) == m.fill_pattern()[5]);
    CHECK(m.read(natural(99999)) == m.fill_pattern()[3]);
}

TEST_CASE("initial n") {
    auto n_of = [](std::size_t len) {
        std::vector<std::uint8_t> p(len, 40);
        return Memory(p).n();
    };
    CHECK(n_of(2) == 10);
    CHECK(n_of(19683) == 10);
    CHECK(n_of(19684) == 11);
    CHECK(n_of(59049) == 11);
}

TEST_CASE("grow_for follows n' = max(n, width(c)+1, width(d)+1)") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    REQUIRE(m.n() == 10);

    // Fast path: small values leave n alone.
    CHECK(m.grow_for(natural(5), V("...2")));
    CHECK(m.n() == 10);
    CHECK(m.grow_for(natural(pow3[9] - 1), natural(0))); // width 9
    CHECK(m.n() == 10);

    // Width 10 needs n = 11.
    CHECK(m.grow_for(natural(pow3[9]), natural(0)));
    CHECK(m.n() == 11);

    // D of width 15 gives n = 16.
    const Value d15 = natural(pow3[14]);
    REQUIRE(d15.width() == 15);
    CHECK(m.grow_for(natural(1), d15));
    CHECK(m.n() == 16);

    // n never decreases.
    CHECK(m.grow_for(natural(1), natural(1)));
    CHECK(m.n() == 16);
    CHECK(m.grow_for(natural(pow3[12]), natural(pow3[3])));
    CHECK(m.n() == 16);

    // C may be the wide one.
    CHECK(m.grow_for(natural(pow3[19]), natural(0))); // width 20
    CHECK(m.n() == 21);

    // Width 38 gives n = 39.
    CHECK(m.grow_for(natural(pow3[37]), natural(0)));
    CHECK(m.n() == 39);

    // Width 39 would need n = 40: refused, n unchanged.
    const Value w39 = natural(pow3[38]);
    REQUIRE(w39.width() == 39);
    CHECK_FALSE(m.grow_for(w39, natural(0)));
    CHECK(m.n() == 39);
    CHECK_FALSE(m.grow_for(natural(0), w39));
    CHECK(m.n() == 39);
}

TEST_CASE("grow_for failure leaves n unchanged when the other operand would grow it") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    CHECK_FALSE(m.grow_for(natural(pow3[14]), natural(pow3[38])));
    CHECK(m.n() == 10);
}

TEST_CASE("grow_for agrees with the width definition on random values") {
    std::mt19937_64 rng(505);
    const std::vector<std::uint8_t> prog = {40, 80};
    for (int iter = 0; iter < 200; ++iter) {
        Memory m(prog);
        unsigned n = 10;
        for (int k = 0; k < 20; ++k) {
            const Value c = random_value(rng, 12 + static_cast<unsigned>(rng() % 28));
            const Value d = random_value(rng, 12 + static_cast<unsigned>(rng() % 28));
            const unsigned want = std::max({n, c.width() + 1, d.width() + 1});
            const bool ok = m.grow_for(c, d);
            CHECK(ok == (want <= 39));
            if (ok) {
                n = want;
            }
            REQUIRE(m.n() == n);
        }
    }
}

TEST_CASE("sparse storage: few writes allocate few pages, reads allocate nothing") {
    std::mt19937_64 rng(606);
    const std::vector<std::uint8_t> prog = {40, 80};
    for (unsigned head = 0; head < 3; ++head) {
        Memory m(prog);
        const std::size_t before = m.page_count();
        const std::uint64_t q = (pow3[30] - 1) / 2;
        const std::uint64_t all_h = head * ((pow3[kTrits] - 1) / 2);
        std::vector<Value> addrs;
        for (int i = 0; i < 10; ++i) {
            const std::uint64_t pick = rng() % (2 * q + 1);
            addrs.push_back(Value::from_raw(all_h + pick - head * q));
        }
        for (const Value a : addrs) {
            m.write(a, natural(9));
        }
        std::vector<std::uint64_t> idxs;
        for (const Value a : addrs) {
            idxs.push_back(a.raw() / kPage);
        }
        std::sort(idxs.begin(), idxs.end());
        idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
        CHECK(m.page_count() - before == idxs.size()); // head 0 pages are far from page 0
        const std::size_t after = m.page_count();
        for (int i = 0; i < 5000; ++i) {
            (void)m.read(random_value(rng));
        }
        CHECK(m.page_count() == after);
        for (const Value a : addrs) {
            CHECK(m.read(a) == natural(9));
        }
    }
}

TEST_CASE("page cache: interleaved access to many pages stays correct") {
    std::mt19937_64 rng(707);
    const auto prog = random_program(rng, 20);
    Memory m(prog);
    constexpr int kPages = 6;
    std::vector<Value> addrs;
    for (int i = 0; i < kPages; ++i) {
        addrs.push_back(Value::from_raw(pow3[39] * (static_cast<std::uint64_t>(i) % 3) +
                                        kPage * (100 + 37 * static_cast<std::uint64_t>(i)) + 11));
    }
    // Reads before any write (absent pages), interleaved.
    for (int r = 0; r < 3; ++r) {
        for (const Value a : addrs) {
            CHECK(m.read(a) == expected_fill(m, a));
        }
    }
    // Write in an interleaved order, with reads of the others in between.
    for (int i = 0; i < kPages; ++i) {
        m.write(addrs[static_cast<std::size_t>(i)], natural(1000 + static_cast<std::uint64_t>(i)));
        for (int j = 0; j < kPages; ++j) {
            const Value want = j <= i ? natural(1000 + static_cast<std::uint64_t>(j))
                                      : expected_fill(m, addrs[static_cast<std::size_t>(j)]);
            CHECK(m.read(addrs[static_cast<std::size_t>(j)]) == want);
        }
    }
    // Random interleaving with a shadow model.
    std::vector<Value> shadow;
    for (const Value a : addrs) {
        shadow.push_back(m.read(a));
    }
    for (int i = 0; i < 20000; ++i) {
        const std::size_t k = rng() % kPages;
        if (rng() % 3 == 0) {
            const Value v = random_value(rng);
            m.write(addrs[k], v);
            shadow[k] = v;
        } else if (m.read(addrs[k]) != shadow[k]) {
            FAIL("shadow mismatch at i=" << i);
        }
    }
}

TEST_CASE("hand-computed fill indices (remainder rule, spec 1.4)") {
    const std::vector<std::uint8_t> prog = {40, 80};
    Memory m(prog);
    const auto& p = m.fill_pattern();
    // ...2 = -1: mod6 = 59048 % 6 = 2 (59048 = 6 * 9841 + 2).
    CHECK(m.read(V("...2")) == p[2]);
    // ...20 = -3 (head 2, trit 0 is 0): 59048 + (0-2) = 59046 = 6 * 9841, mod6 = 0.
    CHECK(m.read(V("...20")) == p[0]);
    // ...2201 (head 2, low trits 201): 59048 + (1-2) + (0-2)*3 + (2-2)*9 = 59041 = 6 * 9840 + 1.
    CHECK(m.read(V("...2201")) == p[1]);
    // Head 1, ...1 : 29524 = 6 * 4920 + 4 -> P[4].
    CHECK(m.read(V("...1")) == p[4]);
    // Head 1, ...1021 (low trits 021): 29524 + (1-1) + (2-1)*3 + (0-1)*9 = 29518 = 6*4919 + 4.
    CHECK(m.read(V("...1021")) == p[4]);
    // Head 1, ...1012: 29524 + (2-1) + (1-1)*3 + (0-1)*9 = 29516 = 6 * 4919 + 2 -> P[2].
    CHECK(m.read(V("...1012")) == p[2]);
    // Head 1 with mod6 = 0: ...12 = 29524 + (2-1) = 29525 (r=5); ...11 = 29524 + 0 = 29524 (r=4);
    // ...10 = 29524 - 1 = 29523 = 6 * 4920 + 3; ...100 -> 29524 - 1 - 3 = 29520 = 6 * 4920 -> P[0].
    CHECK(m.read(V("...100")) == p[0]);
    // Head 0 with mod6 = 0: 1002 = 6 * 167 -> P[0]; natural(1002) is beyond the program.
    CHECK(m.read(natural(1002)) == p[0]);
}

TEST_CASE("move construction and assignment keep the destination intact") {
    std::mt19937_64 rng(808);
    const auto prog = random_program(rng, 30);
    const Value a = natural(5000);
    const Value b = V("...2210");

    Memory src(prog);
    src.write(a, natural(11));
    src.write(b, natural(22)); // both pages are now in the cache
    const auto fill = src.fill_pattern();

    Memory dst(std::move(src));
    // Using the moved-from object must not touch dst's pages.
    src.write(a, natural(99));
    src.write(b, natural(98));
    CHECK(dst.read(a) == natural(11));
    CHECK(dst.read(b) == natural(22));
    CHECK(src.page_count() >= 1); // writes re-materialised pages in src itself
    CHECK(src.read(a) == natural(99));

    Memory dst2(prog);
    dst2 = std::move(dst);
    dst.write(a, natural(77));
    CHECK(dst2.read(a) == natural(11));
    CHECK(dst2.read(b) == natural(22));
    CHECK(dst2.fill_pattern() == fill);

    // The moved-from object can be reassigned and used again.
    dst = Memory(prog);
    dst.write(b, natural(5));
    CHECK(dst.read(b) == natural(5));
    CHECK(dst2.read(b) == natural(22));
}
