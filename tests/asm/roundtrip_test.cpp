// The two reference programs of docs/asm.md 10, the printer, and the print/parse round trip.

#include <doctest/doctest.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "asm/printer.hpp"
#include "asm_test_util.hpp"

using namespace asmtest;

namespace {

// The body of the first ``` fence after the line starting with `heading` in docs/asm.md.
std::string doc_example(const std::string& heading) {
    std::ifstream in(MALM_ASM_DOC);
    REQUIRE_MESSAGE(in.good(), "cannot read " MALM_ASM_DOC);
    std::string line;
    bool found = false;
    bool inside = false;
    std::string body;
    while (std::getline(in, line)) {
        if (!found) {
            found = line.starts_with(heading);
        } else if (!inside) {
            inside = line.starts_with("```");
        } else if (line.starts_with("```")) {
            return body;
        } else {
            body += line + "\n";
        }
    }
    FAIL("example not found: " << heading);
    return {};
}

void check_round_trip(const Program& p, const std::string& what) {
    const std::string text = print_program(p);
    Parsed r = parse_src(text);
    INFO(what << "\n" << text << r.text());
    REQUIRE(!r.diags.has_errors());
    CHECK(same_structure(p, r.program));
    CHECK(print_program(r.program) == text); // the printed form is a fixed point
}

// ---- random ASTs ----

class Gen {
  public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}

    Program program() {
        Program p;
        if (below(3) == 0) {
            p.width = Width{10 + static_cast<unsigned>(below(30)), {}};
        }
        const auto n = 1 + below(5);
        for (std::uint64_t i = 0; i < n; ++i) {
            p.blocks.push_back(block());
        }
        return p;
    }

    Expr expr(int depth) {
        const auto k = below(10);
        if (depth <= 0 || k < 3) {
            return leaf();
        }
        if (k < 5) {
            return Expr::neg(expr(depth - 1));
        }
        static constexpr BinOp kOps[] = {BinOp::Add, BinOp::Sub, BinOp::Shl, BinOp::Shr,
                                         BinOp::Crz};
        return Expr::binary(kOps[below(5)], expr(depth - 1), expr(depth - 1));
    }

  private:
    std::mt19937_64 rng_;

    std::uint64_t below(std::uint64_t n) { return rng_() % n; }

    std::string name() {
        static const char* const kNames[] = {"A",  "B",     "Lbl_1", "x2",
                                             "_y", "ENTRY", "LOOP",  "Zed9"};
        return kNames[below(8)];
    }

    Expr leaf() {
        switch (below(7)) {
        case 0:
            return Expr::literal(*Value::natural(below(10)));
        case 1:
            return Expr::literal(*Value::natural(rng_() % malm::core::pow3[malm::core::kMaxWidth]));
        case 2:
            return Expr::literal(Value::from_raw(rng_() % malm::core::pow3[malm::core::kTrits]));
        case 3:
            return Expr::literal(*Value::natural(below(256)), {}, true);
        case 4:
            return Expr::label(name());
        case 5:
            return Expr::restore(name());
        default:
            return Expr::literal(*Value::from_int(-static_cast<std::int64_t>(below(100))));
        }
    }

    Word word(Section s) {
        Word w;
        if (s == Section::Code) {
            const auto k = below(4);
            if (k == 0) {
                w.kind = Word::Kind::RNop;
                return w;
            }
            static constexpr Op kOps[] = {Op::Nop, Op::Jmp, Op::MovD, Op::Opr,
                                          Op::Rot, Op::In,  Op::Out,  Op::Hlt};
            const auto len = k == 1 ? 1 : 2 + below(5);
            for (std::uint64_t i = 0; i < len; ++i) {
                w.ops.push_back(kOps[below(8)]);
            }
            w.kind = len == 1 ? Word::Kind::Command : Word::Kind::Cycle;
            return w;
        }
        const auto k = below(10);
        if (k == 0) {
            w.kind = Word::Kind::Unknown;
        } else if (k == 1) {
            w.kind = Word::Kind::Unused;
        } else if (k == 2) {
            w.kind = Word::Kind::UseRNop;
            w.use = UseRef{NameRef{name(), {}}, NameRef{name(), {}}};
        } else {
            w.kind = Word::Kind::Expr;
            w.expr = expr(4);
        }
        return w;
    }

    Block block() {
        Block b;
        b.section = below(2) == 0 ? Section::Code : Section::Data;
        b.braced = below(2) == 0;
        if (below(4) == 0) {
            b.offset = Offset{expr(3), {}};
        }
        const auto n = 1 + below(6);
        for (std::uint64_t i = 0; i < n; ++i) {
            Cell c;
            const auto labels = below(3) == 0 ? below(3) : 0;
            for (std::uint64_t j = 0; j < labels; ++j) {
                c.labels.push_back(Label{name(), {}});
            }
            c.word = word(b.section);
            c.starts_line = i == 0 || below(2) == 0;
            b.cells.push_back(std::move(c));
        }
        return b;
    }
};

} // namespace

TEST_CASE("10: both reference programs parse and pass the static checks with no diagnostics") {
    for (const char* heading : {"### 10.1", "### 10.2"}) {
        const std::string src = doc_example(heading);
        REQUIRE(!src.empty());
        const Parsed r = analyze_src(src);
        INFO(heading << "\n" << r.text());
        CHECK(r.diags.items().empty());
        CHECK(!r.program.blocks.empty());
    }
}

TEST_CASE("10.1: hello world has the expected shape") {
    const Parsed r = analyze_src(doc_example("### 10.1"));
    REQUIRE(!r.diags.has_errors());
    const Program& p = r.program;
    REQUIRE(p.blocks.size() == 4);
    CHECK(p.blocks[3].braced);
    CHECK(p.blocks[3].section == Section::Data);
    CHECK(p.blocks[3].cells.size() == 85); // 14 * 6 + HALT
    CHECK(p.blocks[3].cells[0].labels[0].name == "ENTRY");
    CHECK(sexp(*p.blocks[3].cells[1].word.expr) == "(<< 0t2200 0t1)"); // 'H'<<1: 72 = 0t2200
    CHECK(p.blocks[3].cells[2].word.kind == Word::Kind::Expr);
    CHECK(sexp(*p.blocks[3].cells[2].word.expr) == "R_ROT");
    CHECK(p.blocks[3].cells[4].word.kind == Word::Kind::Unused);
}

TEST_CASE("10.2: cat has the expected shape") {
    const Parsed r = analyze_src(doc_example("### 10.2"));
    REQUIRE(!r.diags.has_errors());
    const Program& p = r.program;
    REQUIRE(p.blocks.size() == 4);
    const auto& cells = p.blocks[3].cells;
    REQUIRE(cells.size() == 10);
    CHECK(cells[1].labels[0].name == "LOOP");
    CHECK(sexp(*cells[8].word.expr) == "MOVED");
    CHECK(sexp(*cells[9].word.expr) == "LOOP");
}

TEST_CASE("print: the reference programs round-trip") {
    for (const char* heading : {"### 10.1", "### 10.2"}) {
        const Parsed r = parse_src(doc_example(heading));
        REQUIRE(!r.diags.has_errors());
        check_round_trip(r.program, heading);
    }
}

TEST_CASE("print: canonical layout") {
    const Parsed r = parse_src(
        ".WIDTH 12\n.CODE\nROT: Rot/Nop\nJmp\n\n.DATA\n{\n@C21\nENTRY: R_ROT 'H'<<1 (-1)\n"
        "  ?- ? U_ROT X X: \"hi\"\n}\n");
    REQUIRE(!r.diags.has_errors());
    CHECK(print_program(r.program) == ".WIDTH 12\n"
                                      "\n"
                                      ".CODE\n"
                                      "ROT:    Rot/Nop\n"
                                      "        Jmp\n"
                                      "\n"
                                      ".DATA\n"
                                      "{\n"
                                      "@...21\n"
                                      "ENTRY:  R_ROT 'H'<<1 (-1)\n"
                                      "        ?- ? U_ROT X X: 'h' 'i'\n"
                                      "}\n");
}

TEST_CASE("print: parentheses follow the precedence") {
    const auto check = [](const char* src, const char* printed) {
        const Parsed r = parse_src(std::string(".DATA\nL: ") + src + "\n");
        INFO(src);
        REQUIRE(!r.diags.has_errors());
        CHECK(print_expr(*r.program.blocks[0].cells[0].word.expr) == printed);
        check_round_trip(r.program, src);
    };
    check("1+2<<3", "1+2<<3");
    check("(1+2)<<3", "(1+2)<<3");
    check("1-(2-3)", "1-(2-3)");
    check("(1-2)-3", "1-2-3");
    check("1<<2!3", "1<<2!3");
    check("(1<<2)!3", "(1<<2)!3");
    check("1!(2<<3)", "1!(2<<3)");
    check("-(1+2)", "-(1+2)");
    check("-(-1)", "--1");
    check("-1!2", "-1!2");
    check("-(1!2)", "-(1!2)");
    check("1--2", "1--2");
    check("1-(-2)", "1--2");
    check("A!(B!C)", "A!(B!C)");
    check("((A))", "A");
    check("R_X<<1", "R_X<<1");
}

TEST_CASE("print: a word that starts with a minus is parenthesised") {
    const Parsed r = parse_src(".DATA\nL: ROT (-1) R_ROT (-1)+2 3\n");
    REQUIRE(!r.diags.has_errors());
    CHECK(print_program(r.program) == ".DATA\nL:      ROT (-1) R_ROT (-1+2) 3\n");
    check_round_trip(r.program, "minus");
}

TEST_CASE("print: includes are flattened") {
    const Files f{{"lib.mua", ".CODE\nLIB: Jmp\n"}};
    const Parsed r = parse_src(".DATA\nA: 1\n\n.INCLUDE \"lib.mua\"\n\n.DATA\nB: 2\n", f);
    REQUIRE(!r.diags.has_errors());
    CHECK(print_program(r.program) ==
          ".DATA\nA:      1\n\n.CODE\nLIB:    Jmp\n\n.DATA\nB:      2\n");
    check_round_trip(r.program, "include");
}

TEST_CASE("print: every byte round-trips as a character literal") {
    for (unsigned b = 0; b < 256; ++b) {
        Program p;
        Block blk;
        blk.section = Section::Data;
        Cell c;
        c.word.kind = Word::Kind::Expr;
        c.word.expr = Expr::literal(*Value::natural(b), {}, true);
        blk.cells.push_back(std::move(c));
        p.blocks.push_back(std::move(blk));
        const std::string text = print_program(p);
        INFO("byte " << b << ": " << text);
        const Parsed r = parse_src(text);
        REQUIRE(!r.diags.has_errors());
        CHECK(same_structure(p, r.program));
    }
}

TEST_CASE("lexer: every byte as \\xHH, either case, in characters and strings") {
    const char* digits = "0123456789abcdef";
    for (unsigned b = 0; b < 256; ++b) {
        for (const bool upper : {false, true}) {
            std::string hex = {digits[b >> 4], digits[b & 15]};
            if (upper) {
                for (char& ch : hex) {
                    ch = static_cast<char>(std::toupper(ch));
                }
            }
            const Parsed r = parse_src(".DATA\nL: '\\x" + hex + "' \"\\x" + hex + "\"\n");
            INFO(hex);
            REQUIRE(!r.diags.has_errors());
            REQUIRE(r.program.blocks[0].cells.size() == 2);
            CHECK(r.program.blocks[0].cells[0].word.expr->value.raw() == b);
            CHECK(r.program.blocks[0].cells[1].word.expr->value.raw() == b);
        }
    }
}

TEST_CASE("lexer: random strings with mixed raw and escaped bytes") {
    std::mt19937_64 rng(1234);
    for (int iter = 0; iter < 200; ++iter) {
        std::string bytes;
        std::string lit;
        const auto len = 1 + rng() % 20;
        for (std::uint64_t i = 0; i < len; ++i) {
            const auto b = static_cast<unsigned char>(rng() % 256);
            bytes += static_cast<char>(b);
            const bool raw_ok = b >= 32 && b < 127 && b != '"' && b != '\\';
            if (raw_ok && rng() % 2 == 0) {
                lit += static_cast<char>(b);
            } else if (b == '\n') {
                lit += "\\n";
            } else if (b == '\\' || b == '"' || b == '\'') {
                lit += std::string("\\") + static_cast<char>(b);
            } else {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02X", b);
                lit += buf;
            }
        }
        const Parsed r = parse_src(".DATA\nL: \"" + lit + "\"\n");
        INFO(lit);
        REQUIRE(!r.diags.has_errors());
        REQUIRE(r.program.blocks[0].cells.size() == bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            CHECK(r.program.blocks[0].cells[i].word.expr->value.raw() ==
                  static_cast<unsigned char>(bytes[i]));
        }
    }
}

TEST_CASE("round trip: random ASTs (seeded)") {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Gen g(seed);
        const Program p = g.program();
        check_round_trip(p, "seed " + std::to_string(seed));
    }
}

TEST_CASE("round trip: random ASTs, many more seeds" * doctest::test_suite("slow")) {
    for (std::uint64_t seed = 1000; seed < 11000; ++seed) {
        Gen g(seed);
        const Program p = g.program();
        const std::string text = print_program(p);
        const Parsed r = parse_src(text);
        REQUIRE_MESSAGE(!r.diags.has_errors(), "seed " << seed << "\n" << text << r.text());
        REQUIRE_MESSAGE(same_structure(p, r.program), "seed " << seed << "\n" << text);
    }
}
