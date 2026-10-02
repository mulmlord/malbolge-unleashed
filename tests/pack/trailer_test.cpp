#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "pack/trailer.hpp"

using namespace malm::pack;

namespace {

std::vector<std::uint8_t> tail_of(Trailer t) {
    const auto a = make_trailer(t);
    return {a.begin(), a.end()};
}

} // namespace

TEST_CASE("trailer: layout is MALMEXE1, offset LE, length LE") {
    const auto a = make_trailer({0x0102030405060708ULL, 0x1112131415161718ULL});
    CHECK(std::memcmp(a.data(), "MALMEXE1", 8) == 0);
    const std::uint8_t off[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    const std::uint8_t len[8] = {0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11};
    CHECK(std::memcmp(a.data() + 8, off, 8) == 0);
    CHECK(std::memcmp(a.data() + 16, len, 8) == 0);
}

TEST_CASE("parse_trailer: valid") {
    const auto t = tail_of({1000, 50});
    const auto r = parse_trailer(t, 1000 + 50 + kTrailerSize);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->offset == 1000);
    CHECK((*r)->length == 50);
    // smallest possible: offset 1, empty payload
    const auto r2 = parse_trailer(tail_of({1, 0}), 1 + kTrailerSize);
    REQUIRE(r2.has_value());
    CHECK(r2->has_value());
}

TEST_CASE("parse_trailer: no magic is an ordinary file") {
    std::vector<std::uint8_t> t = tail_of({1000, 50});
    t[7] = '2';
    const auto r = parse_trailer(t, 1074);
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());
    // too short to hold a trailer
    const auto r2 = parse_trailer(std::span<const std::uint8_t>(t).first(10), 10);
    REQUIRE(r2.has_value());
    CHECK_FALSE(r2->has_value());
}

TEST_CASE("parse_trailer: inconsistent fields are errors") {
    // length too long / too short for the file
    CHECK_FALSE(parse_trailer(tail_of({1000, 51}), 1074).has_value());
    CHECK_FALSE(parse_trailer(tail_of({1000, 49}), 1074).has_value());
    // offset 0
    CHECK_FALSE(parse_trailer(tail_of({0, 1050}), 1074).has_value());
    // payload larger than the file
    CHECK_FALSE(parse_trailer(tail_of({1000, 5000}), 1074).has_value());
    // offset + length overflows 2^64
    CHECK_FALSE(parse_trailer(tail_of({~0ULL - 9, 20}), 1074).has_value());
    CHECK_FALSE(parse_trailer(tail_of({~0ULL, ~0ULL}), 1074).has_value());
    // offset overlapping the trailer itself
    CHECK_FALSE(parse_trailer(tail_of({1060, 0}), 1074).has_value());
}
