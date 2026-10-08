// Robustness: arbitrary and mutated input must never crash, hang or trip the sanitizers, and
// every outcome must be well formed.

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "asm/printer.hpp"
#include "asm_test_util.hpp"

using namespace asmtest;

namespace {

const std::string kHello = ".WIDTH 12\n"
                           ".CODE\n"
                           "ROT:    Rot/Nop\n"
                           "        Jmp\n\n"
                           "OUT:    Out/Nop ; comment\n"
                           "        Jmp\n\n"
                           "HALT:   Hlt\n\n"
                           "N: Nop/Nop/Nop/Nop/MovD /* c\n\n */ Jmp\n\n"
                           ".DATA\n{\n"
                           "ENTRY:  ROT 'H'<<1 R_ROT    # c\n"
                           "        OUT ?- R_OUT\n"
                           "        U_N V -(1+2)!3>>4 \"a\\x41\\n\", 7 ROT-1 R_ROT\n"
                           "V:      HALT 0t2101 ...1102 1t0 2t12 C21 EOF\n"
                           "}\n"
                           ".INCLUDE \"lib.mua\"\n"
                           ".DATA @5 W: ?\n";

const Files kFiles{{"lib.mua", ".CODE\nLIB: Rot/Nop\nJmp\n"},
                   {"self.mua", ".CODE\n.INCLUDE \"self.mua\"\n"}};

// Everything one run of the front end may leave behind must be well formed.
void check_outcome(const std::string& src) {
    const Parsed r = analyze_src(src, kFiles);
    for (const Diagnostic& d : r.diags.items()) {
        REQUIRE(d.loc.line >= 1);
        REQUIRE(d.loc.col >= 1);
        REQUIRE(d.loc.file < r.program.files.size());
        REQUIRE(!d.message.empty());
        for (const Note& n : d.notes) {
            REQUIRE(n.loc.line >= 1);
            REQUIRE(n.loc.file < r.program.files.size());
        }
    }
    const std::string text = r.text();
    if (r.diags.has_errors()) {
        REQUIRE(text.find(": error: ") != std::string::npos);
        return;
    }
    // A clean program survives printing and re-parsing (flattened).
    const std::string printed = print_program(r.program);
    const Parsed again = parse_src(printed);
    REQUIRE_MESSAGE(!again.diags.has_errors(), src << "\n--\n" << printed << again.text());
    REQUIRE_MESSAGE(same_structure(r.program, again.program), src << "\n--\n" << printed);
}

std::string random_bytes(std::mt19937_64& rng, std::size_t max_len) {
    static const std::string kAlphabet = "ABCRU_ox01289t.'\"\\;#/*(){}:,@!<>+-?= \n\n\n\t";
    std::string s;
    const auto len = rng() % (max_len + 1);
    for (std::size_t i = 0; i < len; ++i) {
        // Mostly token-like characters, sometimes any byte.
        s += rng() % 5 == 0 ? static_cast<char>(rng() % 256) : kAlphabet[rng() % kAlphabet.size()];
    }
    return s;
}

std::string random_words(std::mt19937_64& rng, std::size_t max_len) {
    static const char* const kWords[] = {
        ".CODE", ".DATA", ".WIDTH", ".OFFSET", ".INCLUDE", "{",     "}",           "@",
        ":",     ",",     "(",      ")",       "/",        "!",     "<<",          ">>",
        "+",     "-",     "?",      "?-",      "A:",       "B:",    "ENTRY:",      "A",
        "B",     "R_A",   "U_A",    "Rot",     "Nop",      "Jmp",   "RNop",        "MovD",
        "C21",   "1",     "0t12",   "...2",    "'x'",      "\"s\"", "\"lib.mua\"", "\"self.mua\"",
        "\n",    "\n\n",  "; c\n",  "/* c */", "12"};
    std::string s;
    const auto n = rng() % (max_len + 1);
    for (std::size_t i = 0; i < n; ++i) {
        s += kWords[rng() % (sizeof kWords / sizeof *kWords)];
        s += rng() % 4 == 0 ? "" : " ";
    }
    return s;
}

std::string mutate(std::mt19937_64& rng, std::string s) {
    static const std::string kInteresting = "'\"\\;#/*(){}:,@!<>+-?. \n\t\x01\x80";
    const auto count = 1 + rng() % 4;
    for (std::uint64_t k = 0; k < count && !s.empty(); ++k) {
        const std::size_t pos = rng() % s.size();
        switch (rng() % 6) {
        case 0:
            s.erase(pos, 1 + rng() % 3);
            break;
        case 1:
            s.insert(pos, 1, kInteresting[rng() % kInteresting.size()]);
            break;
        case 2:
            s[pos] = kInteresting[rng() % kInteresting.size()];
            break;
        case 3:
            s.insert(pos, s.substr(rng() % s.size(), 1 + rng() % 12));
            break;
        case 4:
            s.resize(pos);
            break;
        default:
            std::swap(s[pos], s[rng() % s.size()]);
            break;
        }
    }
    return s;
}

void run_fuzz(std::uint64_t seed, int iterations) {
    std::mt19937_64 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        check_outcome(random_bytes(rng, 160));
        check_outcome(random_words(rng, 40));
        check_outcome(mutate(rng, kHello));
        const std::string junk = "Z " + random_bytes(rng, 60);
        const Parsed r = analyze_src(junk, kFiles);
        REQUIRE_MESSAGE(r.diags.has_errors(), junk); // a stray word before any section
    }
}

} // namespace

TEST_CASE("fuzz: the seed program is clean") {
    const Parsed r = analyze_src(kHello, kFiles);
    INFO(r.text());
    CHECK(r.diags.items().empty());
}

TEST_CASE("fuzz: random and mutated input (bounded)") { run_fuzz(20260502, 250); }

TEST_CASE("fuzz: random and mutated input, many more iterations" * doctest::test_suite("slow")) {
    run_fuzz(7, 20000);
}

TEST_CASE("fuzz: every prefix of a valid program") {
    for (std::size_t n = 0; n <= kHello.size(); ++n) {
        check_outcome(kHello.substr(0, n));
    }
}

namespace {

void pathological(std::size_t big) {
    const std::vector<std::string> inputs = {
        ".DATA\nA: " + std::string(big, '('),
        ".DATA\nA: " + std::string(big, '-') + "1",
        ".DATA\nA: 1" +
            [&] {
                std::string s;
                for (std::size_t i = 0; i < big; ++i) {
                    s += "+1";
                }
                return s;
            }(),
        ".CODE\n" + std::string(big, '{'),
        ".CODE\n" + std::string(big, '}'),
        std::string(big, '/') + "*",
        ".DATA\nA: " + std::string(big, '"'),
        ".DATA\nA: " + std::string(big, '\''),
        ".DATA\n" + std::string(big, ':'),
        ".CODE\nA: " +
            [&] {
                std::string s = "Nop";
                for (std::size_t i = 0; i < big; ++i) {
                    s += "/Nop";
                }
                return s;
            }() +
            "\nJmp\n",
        ".DATA\n" +
            [&] {
                std::string s;
                for (std::size_t i = 0; i < big; ++i) {
                    s += "A: ";
                }
                return s;
            }(),
        std::string(big, '\n') + ".WIDTH",
    };
    for (const std::string& src : inputs) {
        const Parsed r = analyze_src(src, kFiles);
        (void)r.text();
    }
    // An include bomb (every file includes the next one twice) is stopped by the include limit.
    Files f;
    for (int i = 0; i < 40; ++i) {
        const std::string next = ".INCLUDE \"b" + std::to_string(i + 1) + ".mua\"\n";
        f["b" + std::to_string(i) + ".mua"] = ".CODE\n" + next + "\n.CODE\n" + next;
    }
    f["b40.mua"] = ".CODE\nX: Jmp\n";
    const Parsed r = parse_src(".INCLUDE \"b0.mua\"\n", f);
    REQUIRE(r.diags.has_errors());
    CHECK(r.errors().back().find("includes in one program") != std::string::npos);
}

} // namespace

TEST_CASE("fuzz: pathological shapes terminate with diagnostics") { pathological(10000); }

TEST_CASE("fuzz: pathological shapes, much larger" * doctest::test_suite("slow")) {
    pathological(100000);
}
