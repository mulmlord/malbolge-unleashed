// The startup code (issue #15, docs/asm.md 7): the generated text runs on the real VM and leaves
// exactly the image behind (`start` checks every image cell, n, D = ENTRY and C at a Jmp).

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <string>

#include "core/loader.hpp"
#include "layout_test_util.hpp"

using namespace asmtest;

namespace {

// A random value of head 0 or 1 and width at most `max_width`, as a literal (docs/asm.md 4.2).
std::string random_literal(std::mt19937_64& rng, unsigned max_width) {
    const unsigned head = static_cast<unsigned>(rng() % 2);
    const unsigned width = static_cast<unsigned>(rng() % (max_width + 1));
    std::string s = std::to_string(head) + "t";
    for (unsigned i = 0; i < std::max(width, 1u); ++i) {
        s += static_cast<char>('0' + rng() % 3);
    }
    return s;
}

std::string data_program(const std::string& width, const std::string& words) {
    return width + ".CODE\nA: Jmp\n\n.DATA\nENTRY: A " + words + "\n";
}

// Startup cells per image cell.
double cost(const Assembly& a) {
    return static_cast<double>(a.startup.code_cells) / static_cast<double>(a.layout.cells.size());
}

} // namespace

TEST_CASE("random values of heads 0 and 1 and widths up to 30 are built exactly") {
    std::mt19937_64 rng(15);
    for (const unsigned n : {10u, 20u, 31u}) {
        for (int round = 0; round < 3; ++round) {
            CAPTURE(n);
            CAPTURE(round);
            std::string words;
            for (int i = 0; i < 40; ++i) {
                words += random_literal(rng, std::min(30u, n)) + " ";
            }
            const Assembly a = assembled(data_program(".WIDTH " + std::to_string(n) + "\n", words));
            CHECK(a.layout.width == n);
            start(a);
        }
    }
}

TEST_CASE("the default width: the program width is forced from the initial n") {
    // A block at a head-1 address of width 24 needs n >= 25; the text alone starts at n = 10.
    const Assembly a =
        assembled(".DATA\n@1t1" + std::string(24, '0') + " {X: 1t2 7}\nENTRY: A\n.CODE\nA: Jmp\n");
    CHECK(a.layout.width == 25);
    CHECK(initial_width(a.startup.text.size()) < 25);
    start(a);
}

TEST_CASE("width forcing never lands between the text and the base") {
    // Texts just below 3^9 cells with a base above 3^9: a forcing pointer built from the fill
    // cell ~y would have head 0 and land there, where the startup model knows no cells. These
    // sizes failed with "width forcing: no fill cell to rotate" before the forcing used y only.
    for (const unsigned width : {12U, 16U}) {
        for (unsigned n = 176; n <= 232; n += 8) {
            CAPTURE(width);
            CAPTURE(n);
            std::string src = ".WIDTH " + std::to_string(width) +
                              "\n.CODE\nHALT: Hlt\n\n.DATA\nENTRY: HALT\n{\nD:";
            for (unsigned i = 0; i < n; ++i) {
                src += " " + std::to_string(1000 + 7 * i);
            }
            start(assembled(src + "\n}\n"));
        }
    }
}

TEST_CASE("head-1 blocks, code blocks written at run time, and determinism") {
    const std::string src = ".CODE\nX: Rot/Nop\n   Jmp\n\nHALT: Hlt\n\n"
                            ".DATA\n@1t0 {V: 5 1t2 'q'}\n"
                            "ENTRY: X 'q' R_X\n HALT\n";
    const Assembly a = assembled(src);
    CHECK(addr_of(a.layout, "V") == *Value::parse("...10"));
    malm::core::Vm vm = start(a);
    TestIo io;
    CHECK(vm.run(io, vm.steps() + 10).kind == malm::core::RunResult::Kind::Halted);
    CHECK(assembled(src).startup.text == a.startup.text);
}

TEST_CASE("the program text passes the strict loader and ends where the blocks begin") {
    const Assembly a = assembled(data_program("", "1 2 3"));
    const auto loaded = malm::core::load_text(a.startup.text);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value() == a.startup.text);
    CHECK(a.startup.text.size() <= a.base);
    CHECK(a.startup.text[0] == 'b');
    for (const ImageCell& c : a.layout.cells) {
        CHECK((c.addr.head() != 0 || c.addr.raw() >= a.base));
    }
}

TEST_CASE("startup code size per image cell (regression thresholds)") {
    // Bytes, as in text output; full-width random values; the hello world of docs/asm.md 10.1.
    std::mt19937_64 rng(1);
    std::string bytes;
    std::string wide;
    for (int i = 0; i < 200; ++i) {
        bytes += std::to_string(rng() % 256) + " ";
        wide += random_literal(rng, 19) + " ";
    }
    std::string hello = ".CODE\nROT: Rot/Nop\n     Jmp\n\nOUT: Out/Nop\n     Jmp\n\nHALT: Hlt\n\n"
                        ".DATA\nENTRY:";
    for (const char c : std::string("Hello, world!")) {
        hello += std::string(" ROT '") + c + "'<<1 R_ROT\n OUT ?- R_OUT\n";
    }
    hello += " HALT\n";
    struct Case {
        const char* name;
        std::string src;
        double threshold;
    };
    const Case cases[] = {{"bytes, n = 20", data_program(".WIDTH 20\n", bytes), 115},
                          {"width-19 values, n = 20", data_program(".WIDTH 20\n", wide), 160},
                          {"hello world", hello, 88}};
    for (const Case& c : cases) {
        const Assembly a = assembled(c.src);
        MESSAGE(std::string(c.name) << ": " << a.layout.cells.size() << " image cells, "
                                    << a.startup.code_cells << " startup cells, " << cost(a)
                                    << " per image cell (n = " << a.layout.width << ")");
        CHECK(cost(a) <= c.threshold);
    }
}
