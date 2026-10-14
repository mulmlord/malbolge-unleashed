// The HLA language core (issue #19): every statement compiled, assembled and run in the VM, with
// the registers read back from memory afterwards; diagnostics; source locations of the lowering.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "hla_test_util.hpp"

using namespace hlatest;

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

} // namespace

TEST_CASE("mov, bz and bnz on 32-bit words") {
    std::vector<std::uint32_t> values = {0,          1,          0x80,       0x100,
                                         0x8000,     0x10000,    0x800000,   0x1000000,
                                         0x80000000, 0x7FFFFFFF, 0xFFFFFFFF, 0x00FF0000};
    std::mt19937 rng(19);
    for (int i = 0; i < 4; ++i) {
        values.push_back(static_cast<std::uint32_t>(rng()));
    }
    std::string src = "reg x = 7, y\n";
    std::string want;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string k = std::to_string(i);
        src += "reg r" + k + "\n";
        src += "        mov x, " + hex(values[i]) + "\n";
        src += "        mov r" + k + ", x\n";
        src += "        bz x, z" + k + "\n        out 'n'\n        jmp e" + k + "\n";
        src += "z" + k + ":  out 'z'\n";
        src += "e" + k + ":  bnz r" + k + ", n" + k + "\n        out 'Z'\n        jmp f" + k + "\n";
        src += "n" + k + ":  out 'N'\n";
        src += "f" + k + ":\n";
        want += values[i] == 0 ? "zZ" : "nN";
    }
    src += "        mov y, -5\n        mov y, y\n        halt\n";
    const Compiled c = compile(src);
    const Ran r = run(c.assembly);
    CHECK(r.out == want);
    CHECK(r.status == 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        CAPTURE(i);
        CHECK(reg(c, r, "r" + std::to_string(i)) == values[i]);
    }
    CHECK(reg(c, r, "x") == values.back());
    CHECK(reg(c, r, "y") == 0xFFFFFFFBU);
}

TEST_CASE("out and halt take the low byte of a register; registers start at their value") {
    std::mt19937 rng(19);
    std::vector<std::uint32_t> values = {0, 0xFF, 0xFFFFFF00, 0x12345678, 0x80};
    for (int i = 0; i < 16; ++i) {
        values.push_back(static_cast<std::uint32_t>(rng()));
    }
    std::string src = "reg a, b = 0x123456C8, c = 'Q'\n        out c\n";
    std::string want = "Q";
    for (const std::uint32_t v : values) {
        src += "        mov a, " + hex(v) + "\n        out a\n";
        want += static_cast<char>(v & 0xFF);
    }
    src += "        out \"ok\\n\"\n        halt b\n";
    const Compiled c = compile(src);
    const Ran r = run(c.assembly);
    CHECK(r.out == want + "ok\n");
    CHECK(r.status == 0xC8);
    CHECK(reg(c, r, "b") == 0x123456C8U);
    CHECK(reg(c, r, "c") == 'Q');
}

TEST_CASE("in reads a byte into a word; EOF gives 0xFFFFFFFF and takes the branch") {
    const Compiled c = compile("reg a = 9, b = 9, c = 9, d = 9\n"
                               "        in a\n"
                               "        in b\n"
                               "        in c, eof\n"
                               "        halt 1\n"
                               "eof:    in d\n"
                               "        halt 2\n");
    SUBCASE("bytes") {
        const Ran r = run(c.assembly, std::string("\0\xff\x41", 3));
        CHECK(r.status == 1);
        CHECK(reg(c, r, "a") == 0);
        CHECK(reg(c, r, "b") == 0xFF);
        CHECK(reg(c, r, "c") == 0x41);
        CHECK(reg(c, r, "d") == 9);
    }
    SUBCASE("end of input") {
        const Ran r = run(c.assembly, "x");
        CHECK(r.status == 2);
        CHECK(reg(c, r, "a") == 'x');
        CHECK(reg(c, r, "b") == 0xFFFFFFFF);
        CHECK(reg(c, r, "c") == 0xFFFFFFFF);
        CHECK(reg(c, r, "d") == 0xFFFFFFFF);
    }
}

TEST_CASE("halt without a code exits 0, and so does running off the end") {
    for (const char* src : {"halt\n", "out 'a'\n", "", "start:\nend:\n", "jmp l\nl: out 'a'\n"}) {
        CAPTURE(src);
        const Ran r = run(compile(src).assembly);
        CHECK(r.status == 0);
    }
    CHECK(run(compile("halt 255\n").assembly).status == 255);
    CHECK(run(compile("l: halt 'x'\n").assembly).status == 'x');
}

TEST_CASE("labels: fall through, jump back, several on one statement") {
    const Ran r = run(compile("        jmp b\n"
                              "a:      out 'A'\n"
                              "        halt 3\n"
                              "b:\n"
                              "c:      out 'B'\n"
                              "        jmp a\n")
                          .assembly);
    CHECK(r.out == "BA");
    CHECK(r.status == 3);
}

TEST_CASE("outlining: repeated statements share a routine and do what they did inline") {
    // Every kind of statement that is outlined, each at least twice; bz and bnz on one register
    // share a routine whose exits are the call sites' targets.
    const std::string src = "reg a = 5, b = 0x12345678, c, z\n"
                            "ram m[4]\n"
                            "        mov c, m\n"
                            "loop:   add a, a, -1\n"
                            "        xor b, b, a\n"
                            "        sll c, b, 3\n"
                            "        srl c, b, a\n"
                            "        store [m], b\n"
                            "        load c, [m]\n"
                            "        out \"<>\"\n"
                            "        out a\n"
                            "        bz a, done\n"
                            "        bnz z, never\n"
                            "        mov z, 0\n"
                            "        bnz a, loop\n"
                            "never:  halt 9\n"
                            "done:   add a, a, -1\n"
                            "        xor b, b, a\n"
                            "        store [m], b\n"
                            "        mov z, 0\n"
                            "        out \"<>\"\n"
                            "        load c, [m]\n"
                            "        sll c, b, 3\n"
                            "        bz z, end\n"
                            "        halt 8\n"
                            "end:    srl c, b, a\n"
                            "        out a\n"
                            "        bnz z, never\n"
                            "        halt 7\n";
    const Compiled inline_ = compile(src, {.outline = false});
    const Compiled outlined = compile(src);
    const Ran want = run(inline_.assembly);
    const Ran got = run(outlined.assembly);
    CHECK(want.status == 7);
    CHECK(got.status == want.status);
    CHECK(got.out == want.out);
    for (const char* r : {"a", "b", "c", "z"}) {
        CAPTURE(r);
        CHECK(reg(outlined, got, r) == reg(inline_, want, r));
    }
    CHECK(ram_word(got, 0) == ram_word(want, 0));
    CHECK(outlined.assembly.layout.labels.contains("_O0"));
    CHECK(!inline_.assembly.layout.labels.contains("_O0"));
    CHECK(outlined.assembly.startup.text.size() < inline_.assembly.startup.text.size());
    // The first statement outlined: ENTRY is its call.
    CHECK(run(compile("reg a\n        mov a, 4\n        mov a, 4\n        halt a\n").assembly)
              .status == 4);
}

TEST_CASE("generated cells carry the source line of their statement") {
    assembler::MemoryLoader loader({{"t.mhl", "reg a = 1\n\n        out a\n        halt 3\n"}});
    assembler::Diagnostics diags;
    const assembler::Program p = hla::translate("t.mhl", loader, diags);
    REQUIRE(!diags.has_errors());
    REQUIRE(p.files.size() == 1);
    CHECK(p.files[0].path == "t.mhl");
    CHECK(p.width->value == hla::kWidth);
    bool found_entry = false;
    for (const assembler::Block& b : p.blocks) {
        for (const assembler::Cell& cell : b.cells) {
            CHECK(cell.word.loc.file == 0);
            for (const assembler::Label& l : cell.labels) {
                if (l.name == "ENTRY") {
                    found_entry = true;
                }
                if (l.name == "V_a__O") {
                    CHECK(cell.word.loc.line == 1);
                }
            }
        }
        if (b.cells.front().labels.front().name == "ENTRY") {
            for (const assembler::Cell& cell : b.cells) {
                CHECK((cell.word.loc.line == 3 || cell.word.loc.line == 4));
            }
            CHECK(b.cells.back().word.loc.line == 4);
        }
    }
    CHECK(found_entry);
}

TEST_CASE("diagnostics") {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"foo a\n", "t.mhl:1:1: error: unknown instruction 'foo'\n"},
        {"mov a, 1\n", "t.mhl:1:5: error: unknown register 'a'\n"},
        {"reg a\nbz a, a\n", "t.mhl:2:7: error: 'a' is a register, not a label\n"
                             "t.mhl:1:5: note: defined here\n"},
        {"l:\nout l\n", "t.mhl:2:5: error: 'l' is a label, not a register\n"
                        "t.mhl:1:1: note: defined here\n"},
        {"reg a\na: halt\n", "t.mhl:2:1: error: 'a' is already defined\n"
                             "t.mhl:1:5: note: first defined here\n"},
        {"reg a, b, a\n", "t.mhl:1:11: error: 'a' is already defined\n"
                          "t.mhl:1:5: note: first defined here\n"},
        {"reg a\nbz a, nowhere\n", "t.mhl:2:7: error: unknown label 'nowhere'\n"},
        {"out 300\n", "t.mhl:1:5: error: a byte must be 0..255\n"},
        {"halt -1\n", "t.mhl:1:6: error: a byte must be 0..255\n"},
        {"mov 1, 2\n", "t.mhl:1:1: error: expected 'mov <reg>, <reg or value>'\n"},
        {"jmp\n", "t.mhl:1:1: error: expected 'jmp <label>' or 'jmp <reg>'\n"},
        {"in\n", "t.mhl:1:1: error: expected 'in <reg>' or 'in <reg>, <label>'\n"},
        {"reg x = y\n", "t.mhl:1:9: error: unknown name 'y'\n"},
        {"reg x = \"s\"\n", "t.mhl:1:9: error: a register's initial value must be a number, a "
                            "label or a RAM region\n"},
        {"reg x\nreg y = x\n", "t.mhl:2:9: error: 'x' is a register, not a label or a RAM region\n"
                               "t.mhl:1:5: note: defined here\n"},
        {"reg\n", "t.mhl:1:4: error: expected a register name, found end of line\n"},
        {"out \"\"\n", "t.mhl:1:5: error: empty string\n"},
        {"out \"ab\n", "t.mhl:1:5: error: unterminated string\n"},
        {"out '\\q'\n", "t.mhl:1:6: error: invalid escape in a literal\n"},
        {"mov: halt\n", "t.mhl:1:1: error: 'mov' is reserved and cannot be a label\n"},
        {"reg halt\n", "t.mhl:1:5: error: 'halt' is reserved and cannot be a register\n"},
        {"reg a = 0x100000000\n", "t.mhl:1:9: error: number out of range (a 32-bit word)\n"},
        {"reg a = -2147483649\n", "t.mhl:1:9: error: number out of range (a 32-bit word)\n"},
        {"reg a = 12ab\n", "t.mhl:1:9: error: malformed number\n"},
        {"halt @\n", "t.mhl:1:6: error: unexpected character '@'\n"},
        {"halt 1 2\n", "t.mhl:1:8: error: expected ',' or end of line, found number\n"},
        {"halt\nout 999\njmp\n", "t.mhl:2:5: error: a byte must be 0..255\n"
                                 "t.mhl:3:1: error: expected 'jmp <label>' or 'jmp <reg>'\n"},
        // Calls, the stack, switch and RAM (issue #20).
        {"call\n", "t.mhl:1:1: error: expected 'call <label>' or 'call <reg>'\n"},
        {"call nowhere\n", "t.mhl:1:6: error: unknown label 'nowhere'\n"},
        {"ret 1\n", "t.mhl:1:1: error: expected 'ret'\n"},
        {"push \"a\"\n", "t.mhl:1:1: error: expected 'push <reg or value>'\n"},
        {"pop 1\n", "t.mhl:1:1: error: expected 'pop <reg>'\n"},
        {"reg a\nswitch a\n", "t.mhl:2:1: error: expected 'switch <reg>, <label>, ...'\n"},
        {"reg a\nswitch a, 3\n", "t.mhl:2:1: error: expected 'switch <reg>, <label>, ...'\n"},
        {"reg a\nswitch a, a\n", "t.mhl:2:11: error: 'a' is a register, not a label\n"
                                 "t.mhl:1:5: note: defined here\n"},
        {"reg a\nl: switch a" +
             [] {
                 std::string s;
                 for (int i = 0; i < 257; ++i) {
                     s += ", l";
                 }
                 return s;
             }() +
             "\n",
         "t.mhl:2:782: error: a switch has at most 256 labels\n"},
        {"reg a\nload a, 4\n", "t.mhl:2:1: error: expected 'load <reg>, [<address>]'\n"},
        {"reg a\nload [a], a\n", "t.mhl:2:1: error: expected 'load <reg>, [<address>]'\n"},
        {"reg a\nstore a, a\n", "t.mhl:2:1: error: expected 'store [<address>], <reg or value>'\n"},
        {"reg a\nload a, [b\n", "t.mhl:2:11: error: expected ']', found end of line\n"},
        {"reg a\nload a, [[a]]\n",
         "t.mhl:2:10: error: expected a register, a RAM region or an address, found '['\n"},
        {"ram m[2]\nreg a\nload a, [8]\n",
         "t.mhl:3:9: error: the address 8 is not a word of a RAM region (a multiple of 4 "
         "inside a 'ram' declaration)\n"},
        {"ram m[2]\nreg a\nstore [2], 1\n",
         "t.mhl:3:7: error: the address 2 is not a word of a RAM region (a multiple of 4 "
         "inside a 'ram' declaration)\n"},
        {"reg a\nl: load a, [l]\n", "t.mhl:2:12: error: 'l' is a label, not a register or a RAM "
                                    "region\nt.mhl:2:1: note: defined here\n"},
        {"ram m[2]\nmov m, 1\n", "t.mhl:2:5: error: 'm' is a RAM region, not a register\n"
                                 "t.mhl:1:5: note: defined here\n"},
        {"ram m\n", "t.mhl:1:6: error: expected '[', found end of line\n"},
        {"ram m[0]\n", "t.mhl:1:7: error: RAM regions hold 1 to 1048576 words in total\n"},
        {"ram m[1048576]\nram n[1]\n",
         "t.mhl:2:7: error: RAM regions hold 1 to 1048576 words in total\n"},
        {"ram m[1] = 1, 2\n", "t.mhl:1:15: error: more initial values than words in 'm'\n"},
        {"ram m[1] = [1]\n", "t.mhl:1:12: error: a RAM word's initial value must be a number, "
                             "a label or a RAM region\n"},
        {"ram m[2]\nram m[1]\n", "t.mhl:2:5: error: 'm' is already defined\n"
                                 "t.mhl:1:5: note: first defined here\n"},
        {"ram ret[1]\n", "t.mhl:1:5: error: 'ret' is reserved and cannot be a RAM region\n"},
        {"stack 0\n", "t.mhl:1:7: error: the stack holds 1 to 65536 words\n"},
        {"stack 65537\n", "t.mhl:1:7: error: the stack holds 1 to 65536 words\n"},
        {"stack 4\nstack 5\n", "t.mhl:2:1: error: the stack size is already declared\n"
                               "t.mhl:1:1: note: declared here\n"},
        // Arithmetic (issue #21).
        {"reg a\nadd a, a\n", "t.mhl:2:1: error: expected 'add <reg>, <reg>, <reg or value>'\n"},
        {"reg a\nxor 1, a, a\n", "t.mhl:2:1: error: expected 'xor <reg>, <reg>, <reg or value>'\n"},
        {"reg a\nslt a, 1, a\n", "t.mhl:2:1: error: expected 'slt <reg>, <reg>, <reg or value>'\n"},
        {"reg a\nl: sub a, a, l\n", "t.mhl:2:14: error: 'l' is a label, not a register or a RAM "
                                    "region\nt.mhl:2:1: note: defined here\n"},
        {"reg a\nsll a, a, 32\n", "t.mhl:2:11: error: a shift amount must be 0..31\n"},
        {"reg a\nsra a, a, -1\n", "t.mhl:2:11: error: a shift amount must be 0..31\n"},
        {"ram m[1]\nreg a\nsrl a, a, m\n", "t.mhl:3:11: error: 'm' is a RAM region, not a "
                                           "register\nt.mhl:1:5: note: defined here\n"},
        {"reg a\nsextb a, a, a\n", "t.mhl:2:1: error: expected 'sextb <reg>, <reg>'\n"},
        {"reg a\nsexth a, 1\n", "t.mhl:2:1: error: expected 'sexth <reg>, <reg>'\n"},
        // Multiplication and division (issue #22).
        {"reg a\nmul a, a\n", "t.mhl:2:1: error: expected 'mul <reg>, <reg>, <reg or value>'\n"},
        {"reg a\nmulhsu a, 2, a\n",
         "t.mhl:2:1: error: expected 'mulhsu <reg>, <reg>, <reg or value>'\n"},
        {"reg a\nl: divu a, a, l\n", "t.mhl:2:15: error: 'l' is a label, not a register or a "
                                     "RAM region\nt.mhl:2:1: note: defined here\n"},
        {"reg rem\n", "t.mhl:1:5: error: 'rem' is reserved and cannot be a register\n"},
        {"div: halt\n", "t.mhl:1:1: error: 'div' is reserved and cannot be a label\n"},
        {"reg add\n", "t.mhl:1:5: error: 'add' is reserved and cannot be a register\n"},
        {"or: halt\n", "t.mhl:1:1: error: 'or' is reserved and cannot be a label\n"},
        {"a: b: 7\n", "t.mhl:1:7: error: expected a statement, found number\n"},
        {std::string(65, 'x') + ":\n", "t.mhl:1:1: error: identifier longer than 64 characters\n"},
    };
    for (const auto& [src, want] : cases) {
        CAPTURE(src);
        CHECK(diagnostics(src) == want);
    }
    CHECK(diagnostics("ram m[4] = 1, x, m\nstack 1\nreg e = m, f = x\n"
                      "        push x\n        pop e\n        call e\n        call x\n"
                      "        switch e, x, x\n        load e, [m]\n        load e, [12]\n"
                      "        store [e], m\n        jmp e\n        ret\nx:      halt\n") == "");
    CHECK(diagnostics("ram m[2]\nreg a, b\n        add a, b, m\n        sub a, a, -5\n"
                      "        and a, b, 0xFF\n        or a, a, b\n        xor b, a, b\n"
                      "        slt a, b, 'c'\n        sltu a, a, a\n        sll a, b, 31\n"
                      "        srl a, b, 0\n        sra a, a, b\n        sextb a, b\n"
                      "        sexth b, b\n") == "");
    CHECK(diagnostics("ram m[2]\nreg a, b\n        mul a, b, m\n        mulh a, a, -5\n"
                      "        mulhsu a, b, b\n        mulhu b, a, 0xFFFFFFFF\n"
                      "        div a, b, 0\n        divu a, a, b\n        rem a, b, 'c'\n"
                      "        remu b, b, a\n") == "");
    CHECK(diagnostics("reg a = -2147483648, b = 0xFFFFFFFF, c = 0b101, d = '\\x41'\n"
                      "x: y: mov a, b ; comment\n"
                      "# comment\n"
                      "// comment\n"
                      "        out \"\\n\\t\\0\\\\\\\"'\"\n") == "");
}
