// Layout (docs/asm.md 7, issue #14): placement, characters, reserved cells, U_ runs, windows,
// width and the layout-time errors.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "asm/cycles.hpp"
#include "core/ops.hpp"
#include "core/placement.hpp"
#include "layout_test_util.hpp"

using namespace asmtest;
using V = std::vector<std::string>;

namespace {

const char* const kHelloHead = ".CODE\n"
                               "ROT:    Rot/Nop\n"
                               "        Jmp\n"
                               "\n"
                               "OUT:    Out/Nop\n"
                               "        Jmp\n"
                               "\n"
                               "HALT:   Hlt\n"
                               "\n"
                               ".DATA\n";

std::size_t pos(Value a) { return a.mod94(); }

// Every code cell decodes its written cycle at its position, every reserved cell holds a
// character, and no two image cells share an address.
void check_image(const Program& p, const Layout& l) {
    for (std::size_t i = 1; i < l.cells.size(); ++i) {
        REQUIRE(l.cells[i - 1].addr.raw() < l.cells[i].addr.raw());
    }
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const Block& blk = p.blocks[b];
        if (blk.section != Section::Code) {
            continue;
        }
        const Value reserved = plus(l.blocks[b].first, -1);
        CHECK(cell_at(l, reserved).value.raw() >= 33);
        CHECK(cell_at(l, reserved).value.raw() <= 126);
        for (std::size_t c = 0; c < blk.cells.size(); ++c) {
            const Word& w = blk.cells[c].word;
            const Value a = plus(l.blocks[b].first, static_cast<std::int64_t>(c));
            const std::uint8_t ch = char_at(l, a);
            CAPTURE(cycle_text(w.ops));
            CAPTURE(a.to_string());
            if (w.kind == Word::Kind::RNop) {
                CHECK(is_immutable_nop(pos(a), ch));
            } else if (w.kind == Word::Kind::Command) {
                CHECK(op_at(pos(a), ch, 0) == w.ops[0]);
            } else {
                CHECK(w.ops.size() % cycle_length(ch) == 0);
                for (std::size_t k = 0; k < w.ops.size(); ++k) {
                    CHECK(op_at(pos(a), ch, k) == w.ops[k]);
                }
            }
        }
    }
}

Program checked(std::string_view src) {
    Parsed r = analyze_src(src);
    REQUIRE(!r.diags.has_errors());
    return std::move(r.program);
}

} // namespace

TEST_CASE("7: first fit in source order from address 100, at positions where the cycles exist") {
    const std::string src = std::string(kHelloHead) + "ENTRY: ROT 'H'<<1 R_ROT\n       HALT\n";
    const Layout l = laid(src);
    CHECK(l.width == 10);
    // Rot/Nop exists at positions 59 and 63 only; Out/Nop at 25 and 29. Lowest fits, block by
    // block: ROT 153 (59 + 94), OUT 119 (25 + 94), HALT 101 (its reserved cell is 100), ENTRY in
    // the gap after HALT.
    CHECK(addr_of(l, "ROT") == nat(153));
    CHECK(addr_of(l, "OUT") == nat(119));
    CHECK(addr_of(l, "HALT") == nat(101));
    CHECK(addr_of(l, "ENTRY") == nat(102));
    check_image(checked(src), l);

    // The reserved cell is the loadable `o` at its position.
    const ImageCell& r = cell_at(l, nat(152));
    CHECK(r.role == CellRole::Reserved);
    CHECK(r.value == nat(loadable_char(152 % 94, Op::Nop)));
    CHECK(r.loadable);
    // Cycles prefer a loadable character; a single command is the loadable one.
    CHECK(cell_at(l, nat(153)).loadable);
    CHECK(char_at(l, nat(154)) == loadable_char(154 % 94, Op::Jmp));

    // Data words: a label is stored as its address - 1, R_X as the address of X.
    CHECK(cell_at(l, nat(102)).value == nat(152));
    CHECK(malm::core::rotr(cell_at(l, nat(103)).value, 10) == nat('H'));
    CHECK(cell_at(l, nat(104)).value == nat(153));
    CHECK(cell_at(l, nat(105)).value == nat(100));
    CHECK(cell_at(l, nat(102)).role == CellRole::Data);
    CHECK(l.labels.at("ENTRY").section == Section::Data);
    CHECK(l.labels.at("ROT").section == Section::Code);
    CHECK(l.blocks[3].first == nat(102));
    CHECK(l.blocks[3].last == nat(105));
    CHECK(l.find(nat(99)) == nullptr);
}

TEST_CASE("7: layout is deterministic") {
    const std::string src = std::string(kHelloHead) + "ENTRY: ROT 'H'<<1 R_ROT OUT ?- R_OUT HALT\n";
    const Layout a = laid(src);
    const Layout b = laid(src);
    REQUIRE(a.cells.size() == b.cells.size());
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        CHECK(a.cells[i].addr == b.cells[i].addr);
        CHECK(a.cells[i].value == b.cells[i].value);
    }
}

TEST_CASE("7: reserved cells: no back-to-back code blocks, but RNops and ?- slots may hold them") {
    SUBCASE("two code blocks keep a reserved cell between them") {
        const Layout l = laid(".CODE\nA: Jmp\n\nB: Jmp\n\n.DATA\nENTRY: A\n");
        CHECK(addr_of(l, "A") == nat(101));
        CHECK(addr_of(l, "B") == nat(103));
        CHECK(cell_at(l, nat(102)).role == CellRole::Reserved);
    }
    SUBCASE("an RNop of another block serves as the reserved cell") {
        const Layout l = laid(".CODE\nA: Hlt\n   RNop\n\nB: Hlt\n\n.DATA\nENTRY: A\n");
        CHECK(addr_of(l, "A") == nat(101));
        CHECK(addr_of(l, "B") == nat(103));
        const ImageCell& c = cell_at(l, nat(102));
        CHECK(c.role == CellRole::Code);
        CHECK(is_immutable_nop(102 % 94, static_cast<std::uint8_t>(c.value.raw())));
    }
    SUBCASE("a block may sit on ?- slots") {
        const Layout l = laid(".CODE\nA: Jmp\n.DATA\nENTRY: A ?- ?- ?-\n.CODE\nB: Jmp\n");
        CHECK(addr_of(l, "ENTRY") == nat(102));
        CHECK(addr_of(l, "B") == nat(104));
        CHECK(cell_at(l, nat(103)).role == CellRole::Reserved);
        CHECK(l.find(nat(105)) == nullptr);
    }
}

TEST_CASE("5.2: U_ runs: k_max immutable Nops before X, the reserved cell before them") {
    const std::string src = ".CODE\n"
                            "ROT: Rot/Nop\n"
                            "     Jmp\n"
                            ".DATA\n"
                            "ENTRY: U_ROT VAR ?- ?-\n"
                            "VAR:   1\n"
                            "       U_ROT VAR2\n"
                            "VAR2:  2\n";
    const Layout l = laid(src);
    check_image(checked(src), l);
    const Value rot = addr_of(l, "ROT");
    CHECK(rot == nat(153));
    for (const std::int64_t k : {-1, -2}) {
        const ImageCell& c = cell_at(l, plus(rot, k));
        CHECK(c.role == CellRole::RNopRun);
        CHECK(is_immutable_nop(pos(c.addr), static_cast<std::uint8_t>(c.value.raw())));
    }
    CHECK(cell_at(l, plus(rot, -3)).role == CellRole::Reserved);
    // Stored values: addr(X) - 1 - k, with k = 2 and k = 0.
    const Value entry = addr_of(l, "ENTRY");
    CHECK(cell_at(l, entry).value == plus(rot, -3));
    CHECK(cell_at(l, plus(entry, 4)).value == plus(rot, -1));
}

TEST_CASE("7: identical units pack first-fit") {
    const Layout l = laid(".CODE\nF1: Nop/MovD\n    Jmp\n\nF2: Nop/MovD\n    Jmp\n\n"
                          "F3: Nop/MovD\n    Jmp\n\n.DATA\nENTRY: F1\n");
    // MovD in the F/J 2-cycle exists at positions 60 and 64.
    CHECK(addr_of(l, "F1") == nat(154));
    CHECK(addr_of(l, "F2") == nat(158));
    CHECK(addr_of(l, "F3") == nat(248));
}

TEST_CASE("8.2: pinned blocks") {
    SUBCASE("a data block in the head-1 window") {
        const Layout l = laid(".DATA\n@1t0 {X: 5}\nENTRY: A\n.CODE\nA: Jmp\n");
        CHECK(addr_of(l, "X") == *Value::parse("...10"));
        CHECK(cell_at(l, *Value::parse("...10")).value == nat(5));
    }
    SUBCASE("a pinned code block at a position where its cycles exist") {
        const Layout l = laid(".CODE\n@247\nR: Rot/Nop\n   Jmp\n\n.DATA\nENTRY: R\n");
        CHECK(addr_of(l, "R") == nat(247)); // 247 = 59 + 2 * 94
    }
}

TEST_CASE("8.1: the default width grows until every address fits") {
    const Layout l = laid(".DATA\n@19683 {X: 1}\nENTRY: A\n.CODE\nA: Jmp\n"); // 3^9: width 10
    CHECK(l.width == 11);
    const Layout big = laid(".CODE\nA: Jmp\n.DATA\nENTRY: A \"" + std::string(19700, 'a') + "\"\n");
    CHECK(big.width == 11);
}

TEST_CASE("errors: layout") {
    SUBCASE("cycles that exist, but never at consecutive positions") {
        CHECK(layout_errors(".CODE\nX: Rot/Nop\n   Rot/Nop\n   Jmp\n\n.DATA\nENTRY: X\n") ==
              V{"2:1: no position fits every cycle of this code block: its cycles exist only at "
                "positions that do not line up"});
    }
    SUBCASE("pinned in the startup area") {
        CHECK(layout_errors(".DATA\n@99 {X: 1}\nENTRY: A\n.CODE\nA: Jmp\n") ==
              V{"2:1: block pinned at 0t10200 overlaps the startup area, cells 0..99"});
        CHECK(layout_errors(".CODE\n@100\nA: Jmp\n\n.DATA\nENTRY: A\n") ==
              V{"2:1: block pinned at 0t10201 overlaps the startup area, cells 0..99 (with its "
                "reserved cell)"});
    }
    SUBCASE("pinned in head 2") {
        CHECK(layout_errors(".CODE\n@C21\nS: RNop\n   RNop\n\n.DATA\nENTRY: S\n") ==
              V{"2:1: block pinned at ...21 has a head-2 address, which C and D never reach: they "
                "only take values loaded from memory, and no cell holds a head-2 value unless In "
                "returned EOF"});
    }
    SUBCASE("pinned at the last address of a window") {
        // Writing 3^9 - 1 would leave D at 3^9, which has width 10.
        Laid r = lay_out(".WIDTH 10\n.DATA\n@19682 {X: 1}\nENTRY: A\n.CODE\nA: Jmp\n");
        CHECK(r.parsed.errors() == V{"3:1: block pinned at 0t222222222 needs addresses of width "
                                     "10, but program width 10 allows at most 9"});
    }
    SUBCASE("pinned blocks that overlap") {
        Laid r = lay_out(".DATA\n@200 {X: 1 2}\n@201 {Y: 3}\nENTRY: A\n.CODE\nA: Jmp\n");
        CHECK(r.parsed.errors() == V{"3:1: block pinned at 0t21110 overlaps another block"});
        INFO(r.parsed.text());
        CHECK(r.parsed.text().find("t.mua:2:1: note: the other block") != std::string::npos);
    }
    SUBCASE("a pinned cycle at a position where it does not exist") {
        CHECK(layout_errors(".CODE\n@200\nR: Rot/Nop\n   Jmp\n\n.DATA\nENTRY: R\n") ==
              V{"3:4: cycle 'Rot/Nop' does not exist at position 12 (block pinned at 0t21102)"});
    }
    SUBCASE("an explicit width too small for a pinned block") {
        Laid r = lay_out(".WIDTH 10\n.DATA\n@19683 {X: 1}\nENTRY: A\n.CODE\nA: Jmp\n");
        CHECK(r.parsed.errors() == V{"3:1: block pinned at 0t1000000000 needs addresses of width "
                                     "10, but program width 10 allows at most 9"});
        CHECK(r.parsed.text().find("1:1: note: program width set here") != std::string::npos);
    }
    SUBCASE("an explicit width too small for the blocks") {
        CHECK(
            layout_errors(".WIDTH 10\n.CODE\nA: Jmp\n.DATA\nENTRY: A \"" + std::string(19700, 'a') +
                          "\"\n") ==
            V{"5:1: this block does not fit in memory at program width 10 (addresses below 3^9)"});
    }
}

TEST_CASE("errors: data words (4.3, 6)") {
    const auto errs = [](std::string_view data) {
        return layout_errors(".WIDTH 10\n.CODE\nA: Jmp\n.DATA\nENTRY: A " + std::string(data) +
                             "\n");
    };
    CHECK(errs("59049") ==
          V{"5:10: the value 0t10000000000 has width 11, wider than the program width (n = 10)"});
    CHECK(errs("'a'>>11") == V{"5:15: the count of '>>' must be a natural number from 0 to the "
                               "program width (n = 10)"});
    CHECK(errs("'a'<<C2") == V{"5:15: the count of '<<' must be a natural number from 0 to the "
                               "program width (n = 10)"});
    CHECK(errs("59049>>1") ==
          V{"5:10: the operand of '>>' has width 11, wider than the program width (n = 10)"});
    CHECK(errs("0t" + std::string(39, '2') + "+1") ==
          V{"5:51: the result of '+' is out of range (wider than 39 trits)"});
    // Code-reference positions: the first word of a line.
    CHECK(errs("\n 0") == V{"6:2: the first word of a data line is a code reference and must not "
                            "be 0 (jumping through it would encrypt cell 0, the startup Jmp)"});
    CHECK(errs("\n 19682") == V{"6:2: the first word of a data line is a code reference, but its "
                                "landing address 0t1000000000 is wider than n - 1 (n = 10)"});
    // Head 2 (4.3): not even as a plain value, but fine inside an expression.
    CHECK(errs("C2") ==
          V{"5:10: the value ...2 has head 2, which the startup code cannot build: "
            "crz and rotation never turn heads 0 and 1 into head 2 (docs/asm.md 4.3)"});
    // Not at the start of a line, or a value whose landing address fits.
    CHECK(laid(".CODE\nA: Jmp\n.DATA\nENTRY: A 0 19682 C2!'H'\n 1\n").width == 10);
}
