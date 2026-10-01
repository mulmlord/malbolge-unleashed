#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/tables.hpp"

using namespace malm::core;

namespace {

bool is_permutation_of_codes(std::string_view s) {
    std::array<bool, 256> seen{};
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 33 || u > 126 || seen[u]) {
            return false;
        }
        seen[u] = true;
    }
    return s.size() == 94;
}

std::string allowed_at(std::uint32_t pos) {
    std::string out;
    for (std::uint32_t b = 0; b < 256; ++b) {
        if (is_loadable(pos, b)) {
            out += static_cast<char>(b);
        }
    }
    return out; // ascending byte order
}

} // namespace

TEST_CASE("xlat1 and xlat2 are permutations of 33..126") {
    CHECK(is_permutation_of_codes(kXlat1));
    CHECK(is_permutation_of_codes(kXlat2));
}

TEST_CASE("xlat2 cycle lengths are {2,4,5,6,9,68}") {
    std::array<bool, 94> seen{};
    std::vector<int> lengths;
    for (std::uint32_t start = 33; start <= 126; ++start) {
        if (seen[start - 33]) {
            continue;
        }
        int len = 0;
        std::uint32_t x = start;
        do {
            seen[x - 33] = true;
            x = encrypt(x);
            ++len;
        } while (x != start);
        lengths.push_back(len);
    }
    std::sort(lengths.begin(), lengths.end());
    CHECK(lengths == std::vector<int>{2, 4, 5, 6, 9, 68});
}

TEST_CASE("decode table matches spec 3.5") {
    const std::array<std::pair<std::uint32_t, Op>, 8> expect{{{4, Op::Jmp},
                                                              {5, Op::Out},
                                                              {23, Op::In},
                                                              {39, Op::Rot},
                                                              {40, Op::MovD},
                                                              {62, Op::Opr},
                                                              {68, Op::Nop},
                                                              {81, Op::Hlt}}};
    for (std::uint32_t k = 0; k < 94; ++k) {
        Op want = Op::Nop;
        for (const auto& [ek, op] : expect) {
            if (ek == k) {
                want = op;
            }
        }
        CHECK(kOpOfCode[k] == want);
    }
    CHECK(decode(0, 117) == Op::In); // spec 9.1 step 1
    CHECK(decode(1, 39) == Op::MovD);
    CHECK(decode(2, 97) == Op::Out);
    CHECK(decode(4, 64) == Op::Nop);
    CHECK(decode(5, 34) == Op::Rot);
    CHECK(decode(6, 75) == Op::Hlt);
}

TEST_CASE("is_loadable reproduces the research facts") {
    CHECK(allowed_at(0) == "'(>DQbcu");
    CHECK(allowed_at(1) == "&'=CPabt");
    for (std::uint32_t pos = 0; pos < 94; ++pos) {
        CHECK(allowed_at(pos).size() == 8);
    }
    CHECK_FALSE(is_loadable(0, 32));
    CHECK_FALSE(is_loadable(0, 127));
}

TEST_CASE("encrypt examples from spec 9.1") {
    CHECK(encrypt(117) == 111);
    CHECK(encrypt(39) == 116);
    CHECK(encrypt(97) == 46);
    CHECK(encrypt(37) == 103);
    CHECK(encrypt(64) == 81);
    CHECK(encrypt(34) == 122);
}

namespace {

// Spec 3.5 letter to op, written independently of the header.
Op ref_op(char letter) {
    switch (letter) {
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
    case 'v':
        return Op::Hlt;
    default:
        return Op::Nop; // includes 'o'
    }
}

} // namespace

TEST_CASE("decode matches the reference formula for all (c, cell)") {
    for (std::uint32_t c = 0; c < 94; ++c) {
        for (std::uint32_t cell = 33; cell <= 126; ++cell) {
            CHECK(decode(c, cell) == ref_op(kXlat1[(cell - 33 + c) % 94]));
        }
    }
}

TEST_CASE("is_loadable matches the literal spec 4 rule") {
    for (std::uint32_t i = 0; i < 94; ++i) {
        for (std::uint32_t x = 0; x < 256; ++x) {
            const bool want = x >= 33 && x <= 126 &&
                              std::string_view("ji*p</vo").find(kXlat1[(x - 33 + i) % 94]) !=
                                  std::string_view::npos;
            CHECK(is_loadable(i, x) == want);
        }
    }
}

TEST_CASE("tables equal an independent transcription of spec 3.1") {
    CHECK(kXlat1 == "+b(29e*j1VMEKLyC})8&m#~W>qxdRp0wkrUo[D7,XTcA\"lI.v%{gJh4G\\-=O@5`_3i<?Z';"
                    "FNQuY]szf$!BS/|t:Pn6^Ha");
    CHECK(kXlat2 == "5z]&gqtyfr$(we4{WP)H-Zn,[%\\3dL+Q;>U!pJS72FhOA1CB6v^=I_0/"
                    "8|jsb9m<.TVac`uY*MK'X~xDl}REokN:#?G\"i@");
}
