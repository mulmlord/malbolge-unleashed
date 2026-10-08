// One test per diagnostic message, with the exact line:col of the first error.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "asm_test_util.hpp"

using namespace asmtest;
using V = std::vector<std::string>;

namespace {

// All errors of parse + (clean-parse) checks as "line:col: message".
void expect(std::string_view src, const V& want, const Files& files = {}) {
    const Parsed r = analyze_src(src, files);
    INFO(r.text());
    CHECK(r.errors() == want);
}

// A valid program with `extra` appended (extra starts with a blank line).
std::string valid(std::string_view extra) {
    return ".CODE\nJ: Jmp\n\nC: Rot/Nop\nJmp\n\n.DATA\nENTRY: J\n" + std::string(extra);
}
// valid(extra) has kBase lines before `extra`.
constexpr int kBase = 8;
constexpr const char* kMissingEntry =
    "missing ENTRY: the program must define the label ENTRY in a .DATA block";

std::string at(int line, int col, std::string_view msg) {
    return std::to_string(line) + ":" + std::to_string(col) + ": " + std::string(msg);
}

} // namespace

// ---- formatting ----

TEST_CASE("diagnostic format: file:line:col: error: message") {
    const Parsed r = parse_src(".CODE\nA: Rot %\n");
    CHECK(r.text() == "t.mua:2:8: error: unexpected character '%'\n");
}

TEST_CASE("diagnostic notes: first definition of a duplicate label") {
    const Parsed r = analyze_src(valid("\nJ: 1\n"));
    CHECK(r.text() == "t.mua:" + std::to_string(kBase + 2) +
                          ":1: error: duplicate label 'J'\nt.mua:2:1: note: first defined here\n");
}

// ---- lexical errors ----

TEST_CASE("lexer: unexpected character") {
    expect(".CODE\nA: Rot %\n", {at(2, 8, "unexpected character '%'")});
    expect(".CODE\nA: Rot $\n", {at(2, 8, "unexpected character '$'")});
    expect(".CODE\nA: Rot <\n", {at(2, 8, "unexpected character '<'")});
    expect(".CODE\nA: Rot \x01\n", {at(2, 8, "unexpected character '\\x01'")});
    expect(".CODE\nA: Rot \xc3\xa9\n",
           {at(2, 8, "unexpected character '\\xC3'"), at(2, 9, "unexpected character '\\xA9'")});
}

TEST_CASE("lexer: unterminated block comment") {
    expect(".CODE\nA: Rot /* x\n", {at(2, 8, "unterminated block comment")});
}

TEST_CASE("lexer: unterminated character literal") {
    expect(".DATA\nA: 'x\n", {at(2, 4, "unterminated character literal")});
}

TEST_CASE("lexer: unterminated string literal") {
    expect(".DATA\nA: \"xy\n", {at(2, 4, "unterminated string literal")});
}

TEST_CASE("lexer: empty character literal") {
    expect(".DATA\nA: ''\n", {at(2, 4, "empty character literal")});
}

TEST_CASE("lexer: character literal with several characters") {
    expect(".DATA\nA: 'ab'\n", {at(2, 4, "character literal must contain exactly one character")});
}

TEST_CASE("lexer: unknown escape sequence") {
    expect(".DATA\nA: '\\q'\n", {at(2, 5, "unknown escape sequence '\\q'")});
    expect(".DATA\nA: \"a\\qb\"\n", {at(2, 6, "unknown escape sequence '\\q'")});
}

TEST_CASE("lexer: invalid \\x escape") {
    expect(".DATA\nA: '\\xZZ'\n",
           {at(2, 5, "invalid \\x escape: two hexadecimal digits expected")});
    expect(".DATA\nA: '\\x4'\n", {at(2, 5, "invalid \\x escape: two hexadecimal digits expected")});
}

TEST_CASE("lexer: control or non-ASCII character in a literal") {
    const std::string msg = "control or non-ASCII character in literal; use an escape";
    expect(".DATA\nA: \"a\tb\"\n", {at(2, 6, msg)});
    expect(".DATA\nA: \"\xc3\xa9\"\n", {at(2, 5, msg), at(2, 6, msg)});
}

TEST_CASE("lexer: identifier longer than 100 characters") {
    expect(".CODE\n" + std::string(101, 'x') + ": Jmp\n",
           {at(2, 1, "identifier longer than 100 characters")});
    expect(".DATA\nA: R_" + std::string(101, 'x') + "\n",
           {at(2, 4, "identifier longer than 100 characters")});
}

TEST_CASE("lexer: invalid trit digit") {
    expect(".DATA\nA: 0t3\n", {at(2, 4, "invalid trit digit '3' in literal")});
    expect(".DATA\nA: 1t0x\n", {at(2, 4, "invalid trit digit 'x' in literal")});
    expect(".DATA\nA: ...13\n", {at(2, 4, "invalid trit digit '3' in literal")});
}

TEST_CASE("lexer: trit literal without digits") {
    expect(".DATA\nA: 0t\n", {at(2, 4, "trit literal needs at least one digit")});
    expect(".DATA\nA: ...\n", {at(2, 4, "trit literal needs at least one digit")});
    expect(".DATA\nA: ...3\n", {at(2, 4, "trit literal needs at least one digit")});
}

TEST_CASE("lexer: literal wider than 39 trits") {
    expect(".DATA\nA: 0t1" + std::string(39, '0') + "\n",
           {at(2, 4, "literal wider than 39 trits")});
    expect(".DATA\nA: ...12" + std::string(39, '0') + "\n",
           {at(2, 4, "literal wider than 39 trits")});
}

TEST_CASE("lexer: decimal literal too large") {
    expect(".DATA\nA: 4052555153018976267\n",
           {at(2, 4, "decimal literal too large (the limit is 3^39 - 1)")});
    expect(".DATA\nA: 99999999999999999999999\n",
           {at(2, 4, "decimal literal too large (the limit is 3^39 - 1)")});
}

TEST_CASE("lexer: missing whitespace between tokens") {
    expect(".DATA\nA: 12ab\n", {at(2, 6, "missing whitespace between tokens")});
    expect(".DATA\nA: 'a''b'\n", {at(2, 7, "missing whitespace between tokens")});
    expect(".DATA\nA: X\"s\"\n", {at(2, 5, "missing whitespace between tokens")});
    expect(".DATA\nA: ?X\n", {at(2, 5, "missing whitespace between tokens")});
    expect(".CODE\nA: Rot.CODE\n", {at(2, 7, "missing whitespace between tokens")});
}

TEST_CASE("lexer: unknown directive") {
    expect(".FOO\n", {at(1, 1, "unknown directive '.FOO'")});
    expect(".code\nA: Rot\n",
           {at(1, 1, "unknown directive '.code'"),
            at(2, 1, "block outside any section: expected .CODE or .DATA first")});
}

TEST_CASE("lexer: R_ and U_ need a label name") {
    expect(".DATA\nA: R_\n", {at(2, 4, "'R_' must be followed by a label name")});
    expect(".DATA\nA: U_ B\nB: 1\n", {at(2, 4, "'U_' must be followed by a label name")});
    expect(".DATA\nA: R_1\n", {at(2, 4, "'R_' must be followed by a label name")});
}

// ---- sections, blocks, braces ----

TEST_CASE("parser: block outside any section") {
    const char* msg = "block outside any section: expected .CODE or .DATA first";
    expect("A: 1\n", {at(1, 1, msg)});
    expect("{ A: 1 }\n", {at(1, 1, msg)});
    expect("\n.WIDTH 12\nA: Rot\nB: Out\n", {at(3, 1, msg)}); // reported once
}

TEST_CASE("parser: braces do not nest") {
    expect(".CODE\n{ A: Rot { Jmp }\n", {at(2, 10, "braces do not nest")});
}

TEST_CASE("parser: unmatched }") { expect(".CODE\nA: Rot\n}\n", {at(3, 1, "unmatched '}'")}); }

TEST_CASE("parser: unterminated {") { expect(".CODE\n{ A: Rot\n", {at(2, 1, "unterminated '{'")}); }

TEST_CASE("parser: a section directive inside braces") {
    expect(".CODE\n{ A: Rot\n.DATA\n", {at(3, 1, "'.DATA' inside braces; missing '}'")});
    expect(".DATA\n{ A: 1\n.CODE\nB: Hlt\n", {at(3, 1, "'.CODE' inside braces; missing '}'")});
}

TEST_CASE("parser: empty block") {
    expect(".CODE\n{ }\n", {at(2, 1, "empty block")});
    expect(".CODE\n@5\n\nA: Rot\n", {at(2, 1, "empty block")});
}

TEST_CASE("parser: label not followed by a word") {
    expect(".CODE\nA:\n\nB: Rot\n", {at(2, 1, "label 'A' is not followed by a word")});
    expect(".CODE\nA: Rot\nB: C:\n", {at(3, 1, "label 'B' is not followed by a word"),
                                      at(3, 4, "label 'C' is not followed by a word")});
    expect(".CODE\n{ A: Rot\nB: }\n", {at(3, 1, "label 'B' is not followed by a word")});
}

TEST_CASE("parser: reserved words are not labels") {
    expect(".CODE\nNop: Rot\n", {at(2, 1, "'Nop' is a reserved word and cannot be a label")});
    expect(".CODE\nRNop: Rot\n", {at(2, 1, "'RNop' is a reserved word and cannot be a label")});
    expect(".DATA\nC1: 1\n", {at(2, 1, "'C1' is a reserved word and cannot be a label")});
    expect(".DATA\nEOF: 1\n", {at(2, 1, "'EOF' is a reserved word and cannot be a label")});
}

TEST_CASE("parser: labels must not start with R_ or U_") {
    expect(".CODE\nR_X: Rot\n", {at(2, 1, "label names must not start with 'R_' or 'U_'")});
    expect(".CODE\nU_X: Rot\n", {at(2, 1, "label names must not start with 'R_' or 'U_'")});
}

// ---- code words ----

TEST_CASE("parser: expected a command name") {
    expect(".CODE\nA: 5\n", {at(2, 4, "expected a command name, found number")});
    expect(".CODE\nA: Foo\n", {at(2, 4, "expected a command name, found 'Foo'")});
    expect(".CODE\nA: Rot/5\n", {at(2, 8, "expected a command name, found number")});
    expect(".CODE\nA: Rot/\n", {at(3, 1, "expected a command name, found end of file")});
    expect(".CODE\nA: ?\n", {at(2, 4, "expected a command name, found '?'")});
    expect(".CODE\nA: \"s\"\n", {at(2, 4, "expected a command name, found string")});
    expect(".CODE\nA: Rot/\n\nJmp\n", {at(4, 1, "expected a command name, found end of block")});
}

TEST_CASE("parser: RNop is not part of a cycle") {
    expect(".CODE\nA: RNop/Nop\n", {at(2, 4, "'RNop' cannot be part of a cycle")});
    expect(".CODE\nA: Nop/RNop\n", {at(2, 8, "'RNop' cannot be part of a cycle")});
}

// ---- data words ----

TEST_CASE("parser: commands are only allowed in .CODE") {
    expect(".DATA\nA: Rot\n",
           {at(2, 4, "'Rot' is a command and is only allowed in a .CODE block")});
    expect(".DATA\nA: 1+Jmp\n",
           {at(2, 6, "'Jmp' is a command and is only allowed in a .CODE block")});
    expect(".DATA\nA: RNop\n",
           {at(2, 4, "'RNop' is a command and is only allowed in a .CODE block")});
}

TEST_CASE("parser: expected a data word") {
    expect(".DATA\nA: )\n", {at(2, 4, "expected a data word, found ')'")});
    expect(".DATA\nA: ,\n", {at(2, 4, "expected a data word, found ','")});
    expect(".DATA\nA: <<\n", {at(2, 4, "expected a data word, found '<<'")});
    expect(".DATA\nA: 1 /\n", {at(2, 6, "expected a data word, found '/'")});
    expect(".DATA\nA: 1 : 2\n", {at(2, 6, "expected a data word, found ':'")});
}

TEST_CASE("parser: expected an expression") {
    expect(".DATA\nA: 1+\n", {at(3, 1, "expected an expression, found end of file")});
    expect(".DATA\nA: 1 +\n\nB: 2\n", {at(4, 1, "expected an expression, found end of block")});
    expect(".DATA\n{ A: 1 + }\n", {at(2, 10, "expected an expression, found '}'")});
    expect(".DATA\nA: 1 + )\n", {at(2, 8, "expected an expression, found ')'")});
    expect(".DATA\nA: -\n", {at(3, 1, "expected an expression, found end of file")});
    expect(".DATA\nA: (\n", {at(3, 1, "expected an expression, found end of file")});
}

TEST_CASE("parser: expected )") {
    expect(".DATA\nA: (1\n", {at(3, 1, "expected ')', found end of file")});
    expect(".DATA\nA: (1 2)\n", {at(2, 7, "expected ')', found number")});
}

TEST_CASE("parser: whole words cannot be part of an expression") {
    expect(".DATA\nA: 1+?\n",
           {at(2, 6, "'?' is a whole word and cannot be part of an expression")});
    expect(".DATA\nA: (?-)\n",
           {at(2, 5, "'?-' is a whole word and cannot be part of an expression")});
    expect(".DATA\nA: 1 + \"s\"\n",
           {at(2, 8, "string is a whole word and cannot be part of an expression")});
    expect(".DATA\nA: 1-U_X Y\n",
           {at(2, 6, "'U_X' is a whole word and cannot be part of an expression")});
}

TEST_CASE("parser: U_X needs a label operand") {
    expect(".DATA\nA: U_X 5\n", {at(2, 8, "expected a label operand after 'U_X', found number")});
    expect(".DATA\nA: U_X\n",
           {at(3, 1, "expected a label operand after 'U_X', found end of file")});
    expect(".DATA\nA: U_X\nB: 1\n", {at(3, 1, "expected a label operand after 'U_X', found 'B'")});
    expect(".DATA\nA: U_X Rot\n", {at(2, 8, "expected a label operand after 'U_X', found 'Rot'")});
}

TEST_CASE("parser: empty string") { expect(".DATA\nA: \"\"\n", {at(2, 4, "empty string")}); }

// ---- directives ----

TEST_CASE("parser: .WIDTH must come before the first section") {
    expect(".CODE\nA: Rot\n.WIDTH 12\n",
           {at(3, 1, "'.WIDTH' must appear before any section or .INCLUDE")});
    expect(".CODE\n.WIDTH 12\nA: Rot\n",
           {at(2, 1, "'.WIDTH' must appear before any section or .INCLUDE")});
}

TEST_CASE("parser: duplicate .WIDTH") {
    const Parsed r = parse_src(".WIDTH 12\n.WIDTH 13\n.CODE\nA: Rot\n");
    CHECK(r.text() ==
          "t.mua:2:1: error: duplicate '.WIDTH'\nt.mua:1:1: note: first '.WIDTH' is here\n");
}

TEST_CASE("parser: .WIDTH in an included file") {
    const Files f{{"lib.mua", ".WIDTH 12\n.CODE\nL: Hlt\n"}};
    const Parsed r = parse_src(".INCLUDE \"lib.mua\"\n", f);
    CHECK(r.text() == "lib.mua:1:1: error: '.WIDTH' is not allowed in an included file\n"
                      "t.mua:1:1: note: included from here\n");
}

TEST_CASE("parser: .WIDTH needs a decimal number") {
    expect(".WIDTH 0t101\n", {at(1, 8, "'.WIDTH' needs a decimal number, found number")});
    expect(".WIDTH X\n", {at(1, 8, "'.WIDTH' needs a decimal number, found 'X'")});
    expect(".WIDTH\n.CODE\nA: Rot\n", {at(2, 1, "'.WIDTH' needs a decimal number, found '.CODE'")});
}

TEST_CASE("parser: width range") {
    expect(".WIDTH 9\n", {at(1, 8, "width must be between 10 and 39")});
    expect(".WIDTH 40\n", {at(1, 8, "width must be between 10 and 39")});
    expect(".WIDTH 0\n", {at(1, 8, "width must be between 10 and 39")});
}

TEST_CASE("parser: offset directive placement") {
    expect(".CODE\nA: Rot\n@5\n", {at(3, 1, "offset directive must be at the start of a block")});
    expect(".CODE\nA:\n.OFFSET 5\nRot\n",
           {at(3, 1, "offset directive must be at the start of a block")});
    expect(".CODE\n{ A: Rot @5 }\n",
           {at(2, 10, "offset directive must be at the start of a block")});
    expect(".CODE\n@5\n\n{ A: Rot }\n", {at(2, 1, "empty block")});
    expect(".CODE\n@1 @2\nA: Rot\n",
           {at(2, 4, "offset directive must be at the start of a block")});
}

// ---- .INCLUDE ----

TEST_CASE("parser: .INCLUDE inside braces") {
    const Files f{{"lib.mua", ".DATA\nL: 1\n"}};
    expect(".DATA\n{\n.INCLUDE \"lib.mua\"\n}\n",
           {at(3, 1, "'.INCLUDE' is not allowed inside braces")}, f);
}

TEST_CASE("parser: .INCLUDE needs a string path") {
    expect(".INCLUDE foo\n", {at(1, 10, "'.INCLUDE' needs a string path, found 'foo'")});
    expect(".INCLUDE\n", {at(2, 1, "'.INCLUDE' needs a string path, found end of file")});
    expect(".INCLUDE 5\n", {at(1, 10, "'.INCLUDE' needs a string path, found number")});
}

TEST_CASE("parser: .INCLUDE on a line of its own") {
    const Files f{{"lib.mua", ".DATA\nL: 1\n"}};
    expect(".DATA\nA: 1 .INCLUDE \"lib.mua\"\n",
           {at(2, 6, "'.INCLUDE' must be on a line of its own")}, f);
    expect(".INCLUDE \"lib.mua\" .DATA\n", {at(1, 1, "'.INCLUDE' must be on a line of its own")},
           f);
}

TEST_CASE("parser: invalid include path") {
    expect(".INCLUDE \"\"\n", {at(1, 10, "invalid include path")});
    expect(".INCLUDE \"a\\0b\"\n", {at(1, 10, "invalid include path")});
}

TEST_CASE("parser: recursive include") {
    expect(".INCLUDE \"t.mua\"\n", {at(1, 10, "recursive include of 't.mua'")});
    const Files f{{"a.mua", ".INCLUDE \"t.mua\"\n"}};
    const Parsed r = parse_src(".INCLUDE \"a.mua\"\n", f);
    CHECK(r.text() == "a.mua:1:10: error: recursive include of 't.mua'\n"
                      "t.mua:1:1: note: included from here\n");
    const Files g{{"a.mua", ".CODE\n.INCLUDE \"a.mua\"\n"}};
    const Parsed s = parse_src(".INCLUDE \"a.mua\"\n", g);
    CHECK(s.text() == "a.mua:2:10: error: recursive include of 'a.mua'\n"
                      "t.mua:1:1: note: included from here\n");
}

TEST_CASE("parser: cannot open an included file") {
    expect(".INCLUDE \"nope.mua\"\n", {at(1, 10, "cannot open 'nope.mua': no such file")});
}

TEST_CASE("parser: includes nested too deeply") {
    Files f;
    for (int i = 0; i < 70; ++i) {
        f["d" + std::to_string(i) + ".mua"] = ".INCLUDE \"d" + std::to_string(i + 1) + ".mua\"\n";
    }
    f["d63.mua"] = ".CODE\nL: Hlt\n"; // 64 nested levels (d0..d63) are fine
    const Parsed ok = parse_src(".INCLUDE \"d0.mua\"\n", f);
    CHECK(!ok.diags.has_errors());
    f["d63.mua"] = ".INCLUDE \"d64.mua\"\n"; // a 65th level is not
    const Parsed r = parse_src(".INCLUDE \"d0.mua\"\n", f);
    REQUIRE(r.diags.items().size() == 1);
    CHECK(format_diagnostic(r.diags.items()[0], r.program.files)
              .starts_with("d63.mua:1:10: error: includes are nested more than 64 levels deep\n"));
}

TEST_CASE("parser: too many includes") {
    std::string src = ".DATA\n";
    for (int i = 0; i < 1001; ++i) {
        src += ".INCLUDE \"x.mua\"\n";
    }
    const Files f{{"x.mua", ".DATA\nX: 1\n"}};
    const Parsed r = parse_src(src, f);
    REQUIRE(r.diags.items().size() == 1);
    CHECK(r.errors()[0] == at(1002, 10, "more than 1000 includes in one program"));
}

TEST_CASE("parser: errors in an included file name it and the include chain") {
    const Files f{{"lib/a.mua", ".CODE\n.INCLUDE \"b.mua\"\n"}, {"lib/b.mua", ".CODE\nA: Foo\n"}};
    const Parsed r = parse_src(".DATA\n.INCLUDE \"lib/a.mua\"\n", f);
    CHECK(r.text() == "lib/b.mua:2:4: error: expected a command name, found 'Foo'\n"
                      "lib/a.mua:2:1: note: included from here\n"
                      "t.mua:2:1: note: included from here\n");
}

TEST_CASE("check errors in an included file also carry the include chain") {
    const Files f{{"lib.mua", ".CODE\nJ: Jmp\n"}};
    const Parsed r = analyze_src(".DATA\nENTRY: J\nJ: 1\n.INCLUDE \"lib.mua\"\n", f);
    bool found = false;
    for (const Diagnostic& d : r.diags.items()) {
        if (d.message == "duplicate label 'J'") {
            found = true;
            CHECK(format_diagnostic(d, r.program.files) ==
                  "lib.mua:2:1: error: duplicate label 'J'\nt.mua:3:1: note: first defined here\n"
                  "t.mua:4:1: note: included from here\n");
        }
    }
    CHECK(found);
}

TEST_CASE("parser: expressions nested too deeply") {
    const std::string msg = "parentheses and unary minus are nested more than 128 levels deep";
    const std::string hmsg = "expression is more than 256 levels deep";
    expect(".DATA\nA: " + std::string(200, '(') + "1" + std::string(200, ')') + "\n",
           {at(2, 132, msg)});
    expect(".DATA\nA: " + std::string(200, '-') + "1\n", {at(2, 132, msg)});
    std::string chain = ".DATA\nA: 1";
    for (int i = 0; i < 300; ++i) {
        chain += "+1";
    }
    expect(chain + "\n", {at(2, 515, hmsg)});
    // Within the limits all of these are fine.
    std::string ok_chain = ".DATA\nA: 1";
    for (int i = 0; i < 200; ++i) {
        ok_chain += "+1";
    }
    CHECK(!parse_src(ok_chain + "\n").diags.has_errors());
    CHECK(!parse_src(".DATA\nA: " + std::string(100, '(') + "1" + std::string(100, ')') + "\n")
               .diags.has_errors());
}

// ---- recovery ----

TEST_CASE("recovery: independent errors on different lines are all reported") {
    expect(".CODE\nA: 5\nB: Rot\nC: Foo\nD: Out\nE: 1+\n",
           {at(2, 4, "expected a command name, found number"),
            at(4, 4, "expected a command name, found 'Foo'"),
            at(6, 4, "expected a command name, found number")});
}

TEST_CASE("recovery: the rest of the bad line is skipped, no cascade") {
    expect(".DATA\nA: 1 + ) 2 3 4\nB: 5\nC: ) 6 7\n",
           {at(2, 8, "expected an expression, found ')'"),
            at(4, 4, "expected a data word, found ')'")});
}

TEST_CASE("recovery: pending labels survive an error on their word") {
    // `A:` stays pending after the bad word and attaches to the next good one; no second error.
    // `B` is taken as the operand of `+`, as the extent rule says; the `:` is then stray.
    expect(".DATA\nA: 1 +\nB: 2\n", {at(3, 2, "expected a data word, found ':'")});
    expect(".DATA\n{ A: )\n2 }\n", {at(2, 6, "expected a data word, found ')'")});
}

TEST_CASE("recovery: lexical errors do not cascade into the parser") {
    expect(".DATA\nA: 0t3 4 'ab'\nB: \"x\\q\"\n",
           {at(2, 4, "invalid trit digit '3' in literal"),
            at(2, 10, "character literal must contain exactly one character"),
            at(3, 6, "unknown escape sequence '\\q'")});
}

TEST_CASE("recovery: static checks are skipped when the parse had errors") {
    // Without the syntax error the unknown label X and the missing ENTRY would be reported.
    expect(".DATA\nA: X\nB: )\n", {at(3, 4, "expected a data word, found ')'")});
}

TEST_CASE("recovery: a failed block does not hide later blocks") {
    expect(".CODE\nA: Foo\n\nB: Bar\n\n{ C: Baz }\n",
           {at(2, 4, "expected a command name, found 'Foo'"),
            at(4, 4, "expected a command name, found 'Bar'"),
            at(6, 6, "expected a command name, found 'Baz'")});
}

// ---- static checks ----

TEST_CASE("check: duplicate label") {
    expect(".CODE\nA: Jmp\n\nA: Hlt\n", {at(1, 1, kMissingEntry), at(4, 1, "duplicate label 'A'")});
    expect(valid("\nC: 1\n"), {at(kBase + 2, 1, "duplicate label 'C'")});
    expect(valid("\nX: Y: 1\nY: 2\n"), {at(kBase + 3, 1, "duplicate label 'Y'")});
}

TEST_CASE("check: unknown label") {
    expect(valid("\nX: Nope\n"), {at(kBase + 2, 4, "unknown label 'Nope'")});
    expect(valid("\nX: 1+Nope-J\n"), {at(kBase + 2, 6, "unknown label 'Nope'")});
    expect(valid("\nX: R_Nope\n"), {at(kBase + 2, 4, "unknown label 'Nope'")});
    expect(valid("\nX: U_Nope Y\nY: 1\n"), {at(kBase + 2, 4, "unknown label 'Nope'")});
    expect(valid("\nX: U_J Nope\n"), {at(kBase + 2, 8, "unknown label 'Nope'")});
    expect(valid("\nX: 1\n  Nope\n  Nope2\n"),
           {at(kBase + 3, 3, "unknown label 'Nope'"), at(kBase + 4, 3, "unknown label 'Nope2'")});
}

TEST_CASE("check: R_X needs a code label") {
    expect(valid("\nX: R_Y\nY: 1\n"),
           {at(kBase + 2, 4, "'R_Y' needs a code label, but 'Y' names a data cell")});
}

TEST_CASE("check: R_X needs a cycle cell") {
    expect(valid("\nX: R_J\n"),
           {at(kBase + 2, 4, "'R_J' needs a cycle cell, but 'J' is a single command")});
    expect(".CODE\nJ: Jmp\n\nN: RNop\nJmp\n\n.DATA\nENTRY: J R_N\n",
           {at(8, 10, "'R_N' needs a cycle cell, but 'N' is an RNop")});
}

TEST_CASE("check: R_X on an all-Nop cycle is an error") {
    expect(".CODE\nJ: Jmp\n\nN: Nop/Nop/Nop\nJmp\n\n.DATA\nENTRY: J R_N\n",
           {at(8, 10, "'R_N' needs a cycle cell, but 'N' is an all-Nop cycle")});
    expect(".CODE\nJ: Jmp\n\nN: Nop/Rot\nJmp\n\n.DATA\nENTRY: J R_N\n", {});
}

TEST_CASE("check: R_X needs a following cell") {
    expect(".CODE\nJ: Jmp\n\nC: Rot/Nop\n\n.DATA\nENTRY: J R_C\n",
           {at(7, 10, "'R_C' needs a cell after 'C' in its block")});
    // a cell with two labels: the cell after it is what counts
    expect(".CODE\nJ: Jmp\n\nC: D: Rot/Nop\nJmp\n\n.DATA\nENTRY: J R_C R_D\n", {});
}

TEST_CASE("check: R_X inside expressions is checked too") {
    expect(valid("\nX: 1+R_J\n"),
           {at(kBase + 2, 6, "'R_J' needs a cycle cell, but 'J' is a single command")});
}

TEST_CASE("check: U_X needs the first cell of a code block") {
    expect(valid("\nX: U_Y Z\nY: 1\nZ: 2\n"),
           {at(kBase + 2, 4, "'U_Y' needs a code label, but 'Y' names a data cell")});
    expect(".CODE\nJ: Jmp\nK: Hlt\n\n.DATA\nENTRY: J\nX: U_K Z\nZ: 1\n",
           {at(7, 4,
               "'U_K' needs a label on the first cell of a code block, but 'K' is not on "
               "the first cell of its block")});
    expect(".CODE\nJ: K: Jmp\n\n.DATA\nENTRY: J\nX: U_K Z\nZ: 1\n",
           {}); // any label of the first cell
}

TEST_CASE("check: U_X Y needs a data label of a later cell in the same block") {
    expect(valid("\nX: U_C J\n"), {at(kBase + 2, 8, "'U_C': 'J' must be a data label")});
    expect(valid("\nX: U_C Y\n\nY: 1\n"),
           {at(kBase + 2, 8, "'U_C': 'Y' must be defined in the same data block")});
    expect(valid("\nY: 1\nX: U_C Y\n"),
           {at(kBase + 3, 8, "'U_C': 'Y' must be defined after the 'U_' word")});
    expect(valid("\nX: U_C X\n"),
           {at(kBase + 2, 8, "'U_C': 'X' must be defined after the 'U_' word")});
    expect(valid("\nX: U_C Y\nY: 1\n"), {});
    expect(valid("\nX: 1 U_C Y 2\nY: 1\n"), {});
}

TEST_CASE("check: a ?- cell takes no label") {
    expect(valid("\nX: ?-\n"), {at(kBase + 2, 1, "label 'X' cannot name a '?-' cell")});
    expect(valid("\nX: Y: ?-\n"), {at(kBase + 2, 1, "label 'X' cannot name a '?-' cell"),
                                   at(kBase + 2, 4, "label 'Y' cannot name a '?-' cell")});
    expect(valid("\nX: ? ?- ?-\n"), {});
}

TEST_CASE("check: every block starts with a label") {
    expect(valid("\n1 2 3\n"), {at(kBase + 2, 1, "block does not start with a label")});
    expect(".CODE\nJ: Jmp\n\nRot\n\n.DATA\nENTRY: J\n",
           {at(4, 1, "block does not start with a label")});
    expect(valid("\n@5\nX: 1\n"), {});
}

TEST_CASE("check: ENTRY is required") {
    expect(".CODE\nA: Jmp\n", {at(1, 1, kMissingEntry)});
    expect(".DATA\nA: 1\n", {at(1, 1, kMissingEntry)});
}

TEST_CASE("check: ENTRY must be in a .DATA block") {
    expect(".CODE\nENTRY: Jmp\n", {at(2, 1, "ENTRY must be defined in a .DATA block")});
}

TEST_CASE("check: the first word of ENTRY is a code reference") {
    const std::string msg = "the first word of ENTRY must be a code reference (a code label, "
                            "R_<code label> or a U_ word)";
    expect(".DATA\nENTRY: 5\n", {at(2, 8, msg)});
    expect(".DATA\nENTRY: ?\n", {at(2, 8, msg)});
    expect(".DATA\nENTRY: ?-\nX: 1\n",
           {at(2, 1, "label 'ENTRY' cannot name a '?-' cell"), at(2, 8, msg)});
    expect(".DATA\nENTRY: X\nX: 1\n", {at(2, 8, msg)});
    expect(".DATA\nENTRY: J-1\n.CODE\nJ: Jmp\n", {at(2, 8, msg)});
    expect(".DATA\nENTRY: R_C\n.CODE\nC: Rot/Nop\nJmp\n", {});
    expect(".DATA\nENTRY: J\n.CODE\nJ: Jmp\n", {});
    expect(".DATA\nENTRY: U_J Y\nY: 1\n.CODE\nJ: Jmp\n", {});
}

TEST_CASE("check: offset expressions are constant") {
    expect(valid("\n@J\nX: 1\n"),
           {at(kBase + 2, 2, "labels are not allowed in an offset expression")});
    expect(valid("\n@R_J\nX: 1\n"),
           {at(kBase + 2, 2, "labels are not allowed in an offset expression")});
    expect(valid("\n@1<<2\nX: 1\n"),
           {at(kBase + 2, 3, "'<<' is not allowed in an offset expression")});
    expect(valid("\n@(4>>1)+J\nX: 1\n"),
           {at(kBase + 2, 4, "'>>' is not allowed in an offset expression"),
            at(kBase + 2, 9, "labels are not allowed in an offset expression")});
    expect(valid("\n@C21\nX: 1\n"), {});
    expect(valid("\n@1+2!3-(-4)\nX: 1\n"), {});
}

TEST_CASE("check: a cycle must exist at some position") {
    expect(".CODE\nJ: Jmp\n\nA: Rot/Nop/Nop\nJmp\n\n.DATA\nENTRY: J\n",
           {at(4, 4, "cycle 'Rot/Nop/Nop' does not exist at any position")});
    expect(".CODE\nJ: Jmp\n\nA: Jmp/Jmp/Jmp\nJmp\n\n.DATA\nENTRY: J\n",
           {at(4, 4, "cycle 'Jmp/Jmp/Jmp' does not exist at any position")});
    expect(".CODE\nJ: Jmp\n\nA: In/Out\nJmp\n\n.DATA\nENTRY: J\n",
           {at(4, 4, "cycle 'In/Out' does not exist at any position")});
}

TEST_CASE("check: cycles that exist at some position") {
    for (const char* c : {"Rot/Nop", "Nop/Rot", "Out/Nop", "In/Nop", "MovD/Nop", "Nop/MovD",
                          "Opr/Nop", "Nop/Nop", "Nop/Nop/Nop/Nop/MovD", "MovD/Nop/Nop/Nop"}) {
        const Parsed r =
            analyze_src(".CODE\nJ: Jmp\n\nA: " + std::string(c) + "\nJmp\n\n.DATA\nENTRY: J\n");
        INFO(c);
        CHECK(r.text() == "");
    }
}
