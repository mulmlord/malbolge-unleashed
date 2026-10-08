// One test per construct of docs/asm.md (sections 2-5 and 8).

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "asm/lexer.hpp"
#include "asm_test_util.hpp"

using namespace asmtest;
using V = std::vector<std::string>;

namespace {

Program ok(std::string_view src, Files extra = {}) {
    Parsed r = parse_src(src, std::move(extra));
    INFO(r.text());
    REQUIRE(!r.diags.has_errors());
    return std::move(r.program);
}

// The sexp of the one data cell of `.DATA\nL: <expr>`.
std::string expr_of(std::string_view expr) {
    const Program p = ok(".DATA\nL: " + std::string(expr) + "\n");
    REQUIRE(p.blocks.size() == 1);
    REQUIRE(p.blocks[0].cells.size() == 1);
    return sexp(*p.blocks[0].cells[0].word.expr);
}

std::string val(const char* text) { return malm::core::Value::parse(text)->to_string(); }

} // namespace

// ---- 2. Lexical structure ----

TEST_CASE("2: comments ; # // and /* */, and % is not a comment") {
    const Program p = ok(".CODE\n"
                         "A: Rot ; one\n"
                         "B: Out # two\n"
                         "C: In // three\n"
                         "D: /* four */ Hlt /* five\n"
                         "   spanning lines */ E: Jmp\n");
    REQUIRE(p.blocks.size() == 1);
    CHECK(p.blocks[0].cells.size() == 5);
    CHECK(parse_src(".CODE\nA: Rot % not a comment\n").diags.has_errors());
}

TEST_CASE("2: block comments do not nest") {
    // `/* a /* b */` closes at the first `*/`; the trailing `*/` is then an unexpected token.
    CHECK(parse_src(".CODE\nA: Rot /* a /* b */ Jmp */\n").diags.has_errors());
}

TEST_CASE("2: identifiers: letters, digits, underscore, up to 100 characters") {
    const std::string longest(100, 'x');
    const Program p = ok(".CODE\nA_1: Rot\n_b2: Out\n" + longest + ": Jmp\n");
    CHECK(p.blocks[0].cells[0].labels[0].name == "A_1");
    CHECK(p.blocks[0].cells[1].labels[0].name == "_b2");
    CHECK(p.blocks[0].cells[2].labels[0].name == longest);
    CHECK(parse_src(".CODE\n" + std::string(101, 'x') + ": Jmp\n").diags.has_errors());
}

TEST_CASE("2: identifiers and keywords are case-sensitive") {
    CHECK(parse_src(".CODE\nA: rot\n").diags.has_errors());
    CHECK(parse_src(".code\nA: Rot\n").diags.has_errors());
    ok(".CODE\nrot: Rot\nc1: Jmp\nrnop: Hlt\n"); // other case: ordinary labels
}

TEST_CASE("2: labels: several per cell, mid-block, before any word") {
    const Program p = ok(".CODE\nA: B: Rot\nJmp\nC: Out\n");
    REQUIRE(p.blocks[0].cells.size() == 3);
    REQUIRE(p.blocks[0].cells[0].labels.size() == 2);
    CHECK(p.blocks[0].cells[0].labels[0].name == "A");
    CHECK(p.blocks[0].cells[0].labels[1].name == "B");
    CHECK(p.blocks[0].cells[1].labels.empty());
    CHECK(p.blocks[0].cells[2].labels[0].name == "C");
}

TEST_CASE("2: a label may be on its own line before its word") {
    const Program p = ok(".CODE\nA:\nRot\nB:\n  Jmp\n");
    REQUIRE(p.blocks[0].cells.size() == 2);
    CHECK(p.blocks[0].cells[0].labels[0].name == "A");
    CHECK(p.blocks[0].cells[1].labels[0].name == "B");
}

TEST_CASE("2: a blank line ends a block") {
    const Program p = ok(".CODE\nA: Rot\nJmp\n\nB: Out\nJmp\n   \t\nC: Hlt\n");
    REQUIRE(p.blocks.size() == 3);
    CHECK(p.blocks[0].cells.size() == 2);
    CHECK(p.blocks[1].cells.size() == 2);
    CHECK(p.blocks[2].cells.size() == 1);
    CHECK(!p.blocks[0].braced);
}

TEST_CASE("2: a comment-only line is not blank") {
    const Program p = ok(".CODE\nA: Rot\n; comment\nJmp\n// c\nB: Out\n# c\n/* c */\nJmp\n");
    CHECK(p.blocks.size() == 1);
    CHECK(p.blocks[0].cells.size() == 4);
}

TEST_CASE("2: blank lines inside a block comment do not split a block") {
    const Program p = ok(".CODE\nA: Rot\n/* x\n\n\n y */\nJmp\n");
    REQUIRE(p.blocks.size() == 1);
    CHECK(p.blocks[0].cells.size() == 2);
}

TEST_CASE("2: blank-line detection in the lexer") {
    malm::assembler::Diagnostics d;
    const auto toks = lex("a\n\nb\n; c\nc\n/*\n\n*/\nd\n  \ne /* \n\n */ f\n", 0, d);
    REQUIRE(!d.has_errors());
    // a b c d e f EOF
    REQUIRE(toks.size() == 7);
    CHECK_FALSE(toks[0].blank_before);
    CHECK(toks[1].blank_before);       // b
    CHECK_FALSE(toks[2].blank_before); // c: only a comment line between
    CHECK_FALSE(toks[3].blank_before); // d: the blank line is inside a comment
    CHECK(toks[4].blank_before);       // e: whitespace-only line
    CHECK_FALSE(toks[5].blank_before); // f: blank line inside a comment on e's line
}

TEST_CASE("2: braces enclose a block; blank lines inside do not split it") {
    const Program p = ok(".CODE\n{\nA: Rot\n\n\nJmp\n\nB: Out\n}\n\nC: Hlt\n");
    REQUIRE(p.blocks.size() == 2);
    CHECK(p.blocks[0].braced);
    CHECK(p.blocks[0].cells.size() == 3);
    CHECK(!p.blocks[1].braced);
}

TEST_CASE("2: { and } may share a line with other tokens") {
    const Program p = ok(".DATA {\nENTRY: ?\n?- }\n.CODE { A: Rot Jmp }\n");
    REQUIRE(p.blocks.size() == 2);
    CHECK(p.blocks[0].braced);
    CHECK(p.blocks[0].cells.size() == 2);
    CHECK(p.blocks[1].section == malm::assembler::Section::Code);
    CHECK(p.blocks[1].cells.size() == 2);
}

TEST_CASE("2: a brace ends an unbraced block") {
    const Program p = ok(".CODE\nA: Rot\n{ B: Out }\nC: Hlt\n");
    REQUIRE(p.blocks.size() == 3);
    CHECK(p.blocks[1].braced);
}

TEST_CASE("2: words per line: several on one line, line breaks are whitespace") {
    const Program p = ok(".CODE\nA: Rot/Nop Jmp B: Out/Nop\nJmp\n");
    CHECK(p.blocks[0].cells.size() == 4);
    const Program q = ok(".DATA\nL: 1 2 3\n4\n");
    CHECK(data_words(q) == V{val("0t1"), val("0t2"), val("0t10"), val("0t11")});
}

TEST_CASE("2: .CODE and .DATA alternate any number of times and end a block") {
    const Program p = ok(".CODE\nA: Rot\n.DATA\nX: 1\n.CODE\nB: Out\n.DATA\nY: 2\n");
    REQUIRE(p.blocks.size() == 4);
    using S = malm::assembler::Section;
    CHECK(p.blocks[0].section == S::Code);
    CHECK(p.blocks[1].section == S::Data);
    CHECK(p.blocks[2].section == S::Code);
    CHECK(p.blocks[3].section == S::Data);
}

TEST_CASE("2: the file starts with a section; comments, blank lines and .WIDTH may precede it") {
    const Program p = ok("; header\n\n/* c */\n.WIDTH 12\n\n.CODE\nA: Rot\n");
    REQUIRE(p.width);
    CHECK(p.width->value == 12);
    CHECK(p.blocks.size() == 1);
}

TEST_CASE("2: token separation: operators , ( ) @ { } : separate on their own") {
    CHECK(expr_of("A-1") == "(- A 0t1)");
    CHECK(expr_of("A<<1") == "(<< A 0t1)");
    CHECK(expr_of("1+(2)") == "(+ 0t1 0t2)");
    const Program p = ok(".DATA\nL:(1)(2)M:3\n");
    CHECK(data_words(p) == V{val("0t1"), val("0t2"), val("0t10")});
    const Program q = ok(".CODE\n@5\nA:Rot/Nop\n");
    REQUIRE(q.blocks[0].offset);
    CHECK(sexp(q.blocks[0].offset->expr) == val("0t12"));
}

TEST_CASE("2: every block gets its location and the file index") {
    const Program p = ok(".CODE\n\n  A: Rot\n");
    CHECK(p.blocks[0].loc.line == 3);
    CHECK(p.blocks[0].loc.col == 3);
    CHECK(p.blocks[0].cells[0].labels[0].loc.col == 3);
    CHECK(p.blocks[0].cells[0].word.loc.col == 6);
}

// ---- 3. Code blocks ----

TEST_CASE("3.1: single commands: all eight names") {
    const Program p = ok(".CODE\nA: Nop\nJmp\nMovD\nOpr\nRot\nIn\nOut\nHlt\n");
    using malm::assembler::Op;
    const std::vector<Op> want{Op::Nop, Op::Jmp, Op::MovD, Op::Opr,
                               Op::Rot, Op::In,  Op::Out,  Op::Hlt};
    REQUIRE(p.blocks[0].cells.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const auto& w = p.blocks[0].cells[i].word;
        CHECK(w.kind == malm::assembler::Word::Kind::Command);
        REQUIRE(w.ops.size() == 1);
        CHECK(w.ops[0] == want[i]);
    }
}

TEST_CASE("3.1: cycles, Nop included; spaces around / are allowed") {
    using malm::assembler::Op;
    using K = malm::assembler::Word::Kind;
    const Program p = ok(".CODE\nA: MovD/Nop\nB: Nop/Nop/Nop/Nop/MovD\nC: Rot / Nop\nD: Nop/Nop\n");
    CHECK(p.blocks[0].cells[0].word.kind == K::Cycle);
    CHECK(p.blocks[0].cells[0].word.ops == std::vector<Op>{Op::MovD, Op::Nop});
    CHECK(p.blocks[0].cells[1].word.ops ==
          std::vector<Op>{Op::Nop, Op::Nop, Op::Nop, Op::Nop, Op::MovD});
    CHECK(p.blocks[0].cells[2].word.ops == std::vector<Op>{Op::Rot, Op::Nop});
    CHECK(p.blocks[0].cells[3].word.kind == K::Cycle); // all-Nop cycle stays a cycle
}

TEST_CASE("3.1: RNop") {
    const Program p = ok(".CODE\nA: RNop\nB: RNop\n");
    CHECK(p.blocks[0].cells[0].word.kind == malm::assembler::Word::Kind::RNop);
    CHECK(p.blocks[0].cells.size() == 2);
}

TEST_CASE("3.2: a unit: label, code words, Jmp; HALT: Hlt; NOP: Jmp") {
    const Program p = ok(".CODE\nROT: Rot/Nop\n     Jmp\n\nHALT: Hlt\n\nNOP: Jmp\n");
    REQUIRE(p.blocks.size() == 3);
    CHECK(p.blocks[0].cells.size() == 2);
    CHECK(p.blocks[1].cells[0].labels[0].name == "HALT");
    CHECK(p.blocks[2].cells[0].word.ops[0] == malm::assembler::Op::Jmp);
}

// ---- 4. Numbers and constants ----

TEST_CASE("4.2: decimal literals") {
    CHECK(expr_of("0") == val("0t0"));
    CHECK(expr_of("123") == val("0t11120"));
    CHECK(expr_of("007") == val("0t21"));
    CHECK(expr_of("-1") == "(neg " + val("0t1") + ")"); // negative only via unary minus
}

TEST_CASE("4.2: 0t literals, head 0") {
    CHECK(expr_of("0t2101") == val("0t2101"));
    CHECK(expr_of("0t0") == val("0t0"));
    CHECK(expr_of("0t0021") == val("0t21"));
}

TEST_CASE("4.2: 1t and 2t literals, head is the digit before t") {
    CHECK(expr_of("1t00212") == val("...100212"));
    CHECK(expr_of("2t1") == val("...21"));
    CHECK(expr_of("1t1") == val("...1"));
}

TEST_CASE("4.2: ... literals, the first digit is the head") {
    CHECK(expr_of("...1102") == val("...1102"));
    CHECK(expr_of("...2") == val("...2"));
    CHECK(expr_of("...222") == val("...2"));
}

TEST_CASE("4.2: character literals and their escapes") {
    CHECK(expr_of("'A'") == val("0t2102"));
    CHECK(expr_of("'\\n'") == "0t101");
    CHECK(expr_of("'\\r'") == "0t111");
    CHECK(expr_of("'\\t'") == "0t100");
    CHECK(expr_of("'\\0'") == "0t0");
    CHECK(expr_of("'\\\\'") == "0t10102");
    CHECK(expr_of("'\\''") == "0t1110");
    CHECK(expr_of("'\\\"'") == "0t1021");
    CHECK(expr_of("'\\x41'") == expr_of("65"));
    CHECK(expr_of("'\\xff'") == expr_of("255"));
    CHECK(expr_of("'\"'") == expr_of("34"));
    CHECK(expr_of("' '") == expr_of("32"));
}

TEST_CASE("4.2: strings: one cell per byte, same escapes") {
    const Program p = ok(".DATA\nS: \"ab\\n\\x00\\\"\\\\\" T: 1\n");
    CHECK(data_words(p) == V{expr_of("97"), expr_of("98"), expr_of("10"), expr_of("0"),
                             expr_of("34"), expr_of("92"), expr_of("1")});
    CHECK(p.blocks[0].cells[0].labels[0].name == "S"); // the label names the first byte
    CHECK(p.blocks[0].cells[6].labels[0].name == "T");
    CHECK(p.blocks[0].cells[1].word.loc.col ==
          p.blocks[0].cells[0].word.loc.col); // string location
}

TEST_CASE("4.2: string with a separator word between consecutive bytes") {
    const Program p = ok(".DATA\nS: \"abc\", 7 X: \"z\", 9\n");
    CHECK(data_words(p) == V{expr_of("97"), expr_of("7"), expr_of("98"), expr_of("7"),
                             expr_of("99"),
                             expr_of("122")}); // nothing between or after a single byte
    const Program q = ok(".DATA\nS: \"ab\", ? \"cd\", ?-\n");
    CHECK(data_words(q) ==
          V{expr_of("97"), "?", expr_of("98"), expr_of("99"), "?-", expr_of("100")});
}

TEST_CASE("4.2: named constants are 3-adic") {
    CHECK(expr_of("C0") == val("0t0"));
    CHECK(expr_of("C1") == val("...1"));
    CHECK(expr_of("C2") == val("...2"));
    CHECK(expr_of("C21") == val("...21"));
    CHECK(expr_of("C20") == val("...20"));
    CHECK(expr_of("EOF") == val("...2"));
    CHECK(expr_of("C21") != expr_of("C2"));
}

TEST_CASE("4.3: precedence: ! over shifts over + and -, unary minus over !") {
    CHECK(expr_of("1+2<<3") == "(+ 0t1 (<< 0t2 0t10))");
    CHECK(expr_of("1<<2+3") == "(+ (<< 0t1 0t2) 0t10)");
    CHECK(expr_of("1<<2!3") == "(<< 0t1 (! 0t2 0t10))");
    CHECK(expr_of("1!2<<3") == "(<< (! 0t1 0t2) 0t10)");
    CHECK(expr_of("1+2!3") == "(+ 0t1 (! 0t2 0t10))");
    CHECK(expr_of("-1!2") == "(! (neg 0t1) 0t2)");
    CHECK(expr_of("1!-2") == "(! 0t1 (neg 0t2))");
    CHECK(expr_of("A-B>>C") == "(- A (>> B C))");
}

TEST_CASE("4.3: all binary operators are left-associative") {
    CHECK(expr_of("1-2-3") == "(- (- 0t1 0t2) 0t10)");
    CHECK(expr_of("1+2-3+4") == "(+ (- (+ 0t1 0t2) 0t10) 0t11)");
    CHECK(expr_of("1<<2>>3") == "(>> (<< 0t1 0t2) 0t10)");
    CHECK(expr_of("1!2!3") == "(! (! 0t1 0t2) 0t10)");
}

TEST_CASE("4.3: unary minus and parentheses") {
    CHECK(expr_of("--1") == "(neg (neg 0t1))");
    CHECK(expr_of("-(1+2)") == "(neg (+ 0t1 0t2))");
    CHECK(expr_of("(1+2)!3") == "(! (+ 0t1 0t2) 0t10)");
    CHECK(expr_of("1-(2-3)") == "(- 0t1 (- 0t2 0t10))");
    CHECK(expr_of("((7))") == expr_of("7"));
    CHECK(expr_of("1 - -2") == "(- 0t1 (neg 0t2))");
}

TEST_CASE("4.3: leaves: literals, label references, R_X references") {
    CHECK(expr_of("X") == "X");
    CHECK(expr_of("R_X") == "R_X");
    CHECK(expr_of("X-2") == "(- X 0t2)");
    CHECK(expr_of("L1-L2") == "(- L1 L2)");
    CHECK(expr_of("R_A+R_B") == "(+ R_A R_B)");
}

TEST_CASE("4.3: extent across whitespace: an expression continues over a binary operator") {
    const Program p = ok(".DATA\nL: ROT -1 R_ROT\n");
    CHECK(data_words(p) == V{"(- ROT 0t1)", "R_ROT"});
    const Program q = ok(".DATA\nL: ROT (-1) R_ROT\n");
    CHECK(data_words(q) == V{"ROT", "(neg 0t1)", "R_ROT"});
    const Program r = ok(".DATA\nL: A\n  + B\n  << 1\nC: D\n");
    CHECK(data_words(r) == V{"(+ A (<< B 0t1))", "D"});
    const Program s = ok(".DATA\nL: A !\n B\n");
    CHECK(data_words(s) == V{"(! A B)"});
}

TEST_CASE("4.3: ? ?- strings and U_X Y are whole words") {
    const Program p = ok(".DATA\nL: ? -1 ?- -2 U_X Y -3\nY: 1\n");
    CHECK(data_words(p) == V{"?", "(neg 0t1)", "?-", "(neg 0t2)", "U_X Y", "(neg 0t10)", "0t1"});
}

// ---- 5. Data blocks ----

TEST_CASE("5.1: data words: number, expression, label, R_X, U_X Y, ?, ?-") {
    const Program p = ok(".DATA\nENTRY: 5 X-1 X R_X U_X Y ? ?-\nY: 0\n");
    CHECK(data_words(p) == V{val("0t12"), "(- X 0t1)", "X", "R_X", "U_X Y", "?", "?-", "0t0"});
    const auto& u = p.blocks[0].cells[4].word;
    CHECK(u.use->x.name == "X");
    CHECK(u.use->y.name == "Y");
    CHECK(u.use->x.loc.col == 20);
    CHECK(u.use->y.loc.col == 24);
}

TEST_CASE("5.1: U_X Y occupies one cell") {
    const Program p = ok(".DATA\nENTRY: U_X Y\nY: 1\n");
    CHECK(p.blocks[0].cells.size() == 2);
}

TEST_CASE("5.2: R_ and U_ prefixes are tokens, not identifiers") {
    malm::assembler::Diagnostics d;
    const auto t = lex("R_ROT U_NOP Rx Ux R_x_1", 0, d);
    REQUIRE(!d.has_errors());
    using malm::assembler::Tok;
    CHECK(t[0].kind == Tok::RPrefix);
    CHECK(t[0].text == "ROT");
    CHECK(t[1].kind == Tok::UPrefix);
    CHECK(t[1].text == "NOP");
    CHECK(t[2].kind == Tok::Ident);
    CHECK(t[3].kind == Tok::Ident);
    CHECK(t[4].kind == Tok::RPrefix);
}

TEST_CASE("5.3: ENTRY is an ordinary label to the parser") {
    const Program p = ok(".DATA\nENTRY: R_X\n");
    CHECK(p.blocks[0].cells[0].labels[0].name == "ENTRY");
}

// ---- 8. Directives ----

TEST_CASE("8.1: .WIDTH n, 10 to 39") {
    CHECK(ok(".WIDTH 10\n.CODE\nA: Rot\n").width->value == 10);
    CHECK(ok(".WIDTH 39\n.CODE\nA: Rot\n").width->value == 39);
    CHECK(!ok(".CODE\nA: Rot\n").width);
}

TEST_CASE("8.2: @expr and .OFFSET at the start of a block") {
    const Program p =
        ok(".CODE\n@C21\nSLED: RNop\nRNop\n\n.OFFSET 100+5\nB: Hlt\n\n{ @7 C: Hlt }\n");
    REQUIRE(p.blocks.size() == 3);
    REQUIRE(p.blocks[0].offset);
    CHECK(sexp(p.blocks[0].offset->expr) == val("...21"));
    CHECK(p.blocks[0].cells.size() == 2);
    CHECK(sexp(p.blocks[1].offset->expr) == "(+ " + val("0t10201") + " " + val("0t12") + ")");
    CHECK(sexp(p.blocks[2].offset->expr) == val("0t21"));
    CHECK(p.blocks[2].braced);
}

TEST_CASE("8.2: an offset expression may use the extent rule across lines") {
    const Program p = ok(".CODE\n@1\n+ 2\nA: Hlt\n");
    CHECK(sexp(p.blocks[0].offset->expr) == "(+ 0t1 0t2)");
}

TEST_CASE("8.3: .INCLUDE expands in place; the section is set by the included file") {
    const Files f{{"lib.mua", ".CODE\nLIB: Rot/Nop\nJmp\n"}};
    const Program p = ok(".DATA\nA: 1\n\n.INCLUDE \"lib.mua\"\n\nB: 2\n", f);
    REQUIRE(p.blocks.size() == 3);
    using S = malm::assembler::Section;
    CHECK(p.blocks[0].section == S::Data);
    CHECK(p.blocks[1].section == S::Code);
    CHECK(p.blocks[1].cells[0].labels[0].name == "LIB");
    CHECK(p.blocks[2].section == S::Data); // the including file keeps its section
    CHECK(p.blocks[2].cells[0].labels[0].name == "B");
    REQUIRE(p.files.size() == 2);
    CHECK(p.files[1].path == "lib.mua");
    REQUIRE(p.files[1].included_from);
    CHECK(p.files[1].included_from->line == 4);
    CHECK(p.blocks[1].loc.file == 1); // include provenance
    CHECK(p.blocks[0].loc.file == 0);
}

TEST_CASE("8.3: .INCLUDE ends the current unbraced block, like a blank line") {
    const Files f{{"lib.mua", ".DATA\nL: 1\n"}};
    const Program p = ok(".CODE\nA: Rot\n.INCLUDE \"lib.mua\"\nB: Out\n", f);
    REQUIRE(p.blocks.size() == 3);
    CHECK(p.blocks[0].cells.size() == 1);
    CHECK(p.blocks[2].section == malm::assembler::Section::Code);
}

TEST_CASE("8.3: includes resolve relative to the including file, nest, and may repeat") {
    const Files f{{"sub/a.mua", ".CODE\n.INCLUDE \"b.mua\"\nA: Rot\n"},
                  {"sub/b.mua", ".CODE\nB: Out\n"},
                  {"c.mua", ".DATA\nC: 1\n"}};
    const Program p = ok(".INCLUDE \"sub/a.mua\"\n.INCLUDE \"c.mua\"\n.INCLUDE \"c.mua\"\n", f);
    REQUIRE(p.blocks.size() == 4);
    CHECK(p.blocks[0].cells[0].labels[0].name == "B");
    CHECK(p.blocks[1].cells[0].labels[0].name == "A");
    CHECK(p.blocks[2].cells[0].labels[0].name == "C");
    CHECK(p.blocks[3].cells[0].labels[0].name == "C"); // the same file twice in sequence
    CHECK(p.files.size() == 5);
    CHECK(p.files[2].path == "sub/b.mua");
    CHECK(p.files[2].included_from->file == 1);
}

TEST_CASE("8.3: ../ and ./ in include paths are normalised") {
    const Files f{{"lib/x.mua", ".CODE\n.INCLUDE \"../y.mua\"\n"}, {"y.mua", ".DATA\nY: 1\n"}};
    const Program p = ok(".INCLUDE \"./lib/x.mua\"\n", f);
    REQUIRE(p.blocks.size() == 1);
    CHECK(p.files[1].path == "./lib/x.mua"); // as written
    CHECK(p.files[2].path == "./lib/../y.mua");
}

TEST_CASE("8.3: includes before the first section are fine") {
    const Files f{{"lib.mua", ".CODE\nL: Hlt\n"}};
    const Program p = ok(".WIDTH 12\n.INCLUDE \"lib.mua\"\n.DATA\nA: 1\n", f);
    CHECK(p.blocks.size() == 2);
}

TEST_CASE("8.2: @expr may stand directly before the { of a braced block, or right after it") {
    const Program p = ok(".CODE\n@C21 { SLED: RNop RNop }\n\n{ @C21 SLED2: RNop RNop }\n\n"
                         ".OFFSET 7 {\nB: Hlt\n}\n");
    REQUIRE(p.blocks.size() == 3);
    for (const auto& b : p.blocks) {
        CHECK(b.braced);
        REQUIRE(b.offset);
    }
    CHECK(p.blocks[0].cells.size() == 2);
    CHECK(sexp(p.blocks[0].offset->expr) == sexp(p.blocks[1].offset->expr));
    CHECK(sexp(p.blocks[2].offset->expr) == val("0t21"));
    CHECK(p.blocks[2].cells.size() == 1);
    CHECK(same_structure(ok(".CODE\n@C21 { A: RNop }\n"), ok(".CODE\n{ @C21 A: RNop }\n")));
}
