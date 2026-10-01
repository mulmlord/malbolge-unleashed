#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/loader.hpp"

namespace {

using malm::core::load_text;
using Cells = std::vector<std::uint8_t>;

auto load(const std::string& text, std::size_t max_cells = malm::core::kUnleashedMaxCells) {
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    return load_text(bytes, max_cells);
}

} // namespace

TEST_CASE("spec examples") {
    CHECK(load("(P").value() == Cells{40, 80});
    CHECK(load("ubO").value() == Cells{117, 98, 79});                      // /<v  (spec 9.4)
    CHECK(load("u'a%@\"K").value() == Cells{117, 39, 97, 37, 64, 34, 75}); // spec 9.1
    CHECK(load("(&&%M").value() == Cells{40, 38, 38, 37, 77});             // spec 9.2
}

TEST_CASE("whitespace is skipped and does not occupy a cell") {
    // If whitespace counted as a cell, P would be at position 4 where it is not loadable.
    CHECK(load(" (\n P").value() == Cells{40, 80});
    CHECK(load("\t(\v\f\r P\n").value() == Cells{40, 80});
}

TEST_CASE("E_LOAD: byte not loadable at its position") {
    auto r = load("A");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 0);
    CHECK_FALSE(r.error().reason.empty());

    r = load("(A");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 1);

    // Position counts stored cells only: this '(' is cell 2, not loadable there.
    r = load("(P(");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 2);

    // Offsets count skipped bytes.
    r = load("(P ?");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 3);
}

TEST_CASE("E_LOAD: bytes outside 33..126") {
    auto r = load(" (\t\x7f"
                  "P");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 3); // 127

    r = load("(P\x01");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 2);

    r = load("  (\xc8P"); // byte > 126
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 3);

    r = load(std::string("(P\0", 3));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 2);
}

TEST_CASE("E_LOAD: fewer than 2 cells reports the input length") {
    auto r = load("");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 0);

    r = load(" \n\t");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 3);

    r = load(" ( \n");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 4);
}

TEST_CASE("E_LOAD: more cells than max_cells") {
    CHECK(load("(P", 2).has_value());
    // "(P" followed by a third loadable cell; classic mode will pass 2 here to check the rule.
    const auto three = load("ubO", 2);
    REQUIRE_FALSE(three.has_value());
    CHECK(three.error().offset == 2);
}
