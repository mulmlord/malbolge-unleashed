// Resource limits, the file-system loader, include identity, blank lines in braces, line grouping
// and the other behaviours added after the review of #13 (docs/asm.md 2, 8.1-8.3, 9).

#include <doctest/doctest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "asm/printer.hpp"
#include "asm_test_util.hpp"

using namespace asmtest;
namespace fs = std::filesystem;
using V = std::vector<std::string>;

namespace {

std::string at(int line, int col, const std::string& msg) {
    return std::to_string(line) + ":" + std::to_string(col) + ": " + msg;
}

Parsed parse_limited(std::string_view src, const ParseLimits& lim, Files extra = {}) {
    extra["t.mua"] = std::string(src);
    MemoryLoader loader(std::move(extra));
    Parsed r;
    r.program = analyze("t.mua", loader, r.diags, lim);
    return r;
}

// A fresh directory under the temp dir, removed on destruction.
struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("malm_asm_unit_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string write(const std::string& name, const std::string& text) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream(path / name, std::ios::binary) << text;
        return (path / name).string();
    }
};

Parsed analyze_fs(const std::string& path) {
    FsLoader loader;
    Parsed r;
    r.program = analyze(path, loader, r.diags);
    return r;
}

} // namespace

// ---- resource limits ----

TEST_CASE("limits: total source size, one error, parsing stops") {
    ParseLimits lim;
    lim.max_source_bytes = 60;
    const std::string big = ".DATA\nA: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20\nB: )\n";
    CHECK(parse_limited(big, lim).errors() ==
          V{at(1, 1, "source text exceeds the limit of 60 bytes")});
    // The sum over all files counts; the include that crosses the limit is the error site.
    const Files f{{"lib.mua", ".CODE\nL: Hlt\n; padding padding\n"}};
    const std::string main = ".INCLUDE \"lib.mua\"\n.DATA\nX: )\n";
    CHECK(main.size() + f.at("lib.mua").size() > 60 - 10);
    lim.max_source_bytes = main.size() + f.at("lib.mua").size() - 1;
    CHECK(parse_limited(main, lim, f).errors() ==
          V{at(1, 10,
               "source text exceeds the limit of " + std::to_string(lim.max_source_bytes) +
                   " bytes")});
    // Exactly at the limit is fine.
    lim.max_source_bytes = 5;
    CHECK(parse_limited(".CODE", lim).errors().at(0).starts_with("1:1: missing ENTRY"));
    CHECK(parse_limited(".CODEX", lim).errors() ==
          V{at(1, 1, "source text exceeds the limit of 5 bytes")});
}

TEST_CASE("limits: total cell count, strings count one cell per byte") {
    ParseLimits lim;
    lim.max_cells = 10;
    const std::string msg = "program exceeds the limit of 10 cells";
    CHECK(parse_limited(".DATA\nA: 1 2 3 4 5 6 7 8 9 10 11 12\nB: )\n", lim).errors() ==
          V{at(2, 25, msg)});
    CHECK(parse_limited(".DATA\nA: \"abcdefghijk\"\nB: )\n", lim).errors() == V{at(2, 4, msg)});
    // With a separator a 6-byte string takes 11 cells.
    CHECK(parse_limited(".DATA\nA: \"abcdef\", 0\n", lim).errors() == V{at(2, 4, msg)});
    // The count is over all blocks and files.
    const Files f{{"lib.mua", ".DATA\nL: 1 2 3 4 5 6\n"}};
    CHECK(parse_limited(".INCLUDE \"lib.mua\"\n.INCLUDE \"lib.mua\"\n", lim, f).errors() ==
          V{at(2, 12, msg)});
    // Exactly the limit is fine (the checks then complain about ENTRY only).
    CHECK(parse_limited(".DATA\nA: 1 2 3 4 5 6 7 8 9 10\n", lim)
              .errors()
              .at(0)
              .starts_with("1:1: missing"));
}

TEST_CASE("limits: the defaults are 64 MiB and 4 Mi cells") {
    const ParseLimits lim;
    CHECK(lim.max_source_bytes == 64u * 1024 * 1024);
    CHECK(lim.max_cells == 4u * 1024 * 1024);
    CHECK(lim.max_include_depth == 64);
    CHECK(lim.max_includes == 1000);
}

TEST_CASE("limits: cells are small") {
    CHECK(sizeof(Word) <= 64);
    CHECK(sizeof(Cell) <= 96);
}

TEST_CASE("limits: large input is linear" * doctest::test_suite("slow")) {
    const auto t0 = std::chrono::steady_clock::now();
    // 4 Mi - 1 cells in plain words, the last one a string.
    std::string src = ".DATA\nENTRY: ";
    src.reserve(9u << 20);
    for (std::size_t i = 0; i < (std::size_t{4} << 20) - 2; ++i) {
        src += "1 ";
    }
    src += "\"a\"\n";
    const Parsed r = parse_src(src);
    CHECK(r.errors().empty()); // exactly at the limit
    CHECK(r.program.blocks.at(0).cells.size() == (std::size_t{4} << 20) - 1);
    src.insert(src.size() - 1, " 2 3");
    const Parsed over = parse_src(src);
    REQUIRE(over.errors().size() == 1);
    CHECK(over.errors()[0].ends_with("program exceeds the limit of 4194304 cells"));
    // one 3 MiB line of junk
    CHECK(parse_src(".DATA\nA: " + std::string(std::size_t{3} << 20, ')') + "\n").errors().size() ==
          1);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 120);
}

// ---- the file-system loader ----

TEST_CASE("FsLoader: a directory, a FIFO and an unreadable file are errors") {
    TempDir d;
    fs::create_directories(d.path / "dir");
    const std::string dir = (d.path / "dir").string();
    CHECK(analyze_fs(dir).diags.items().at(0).message ==
          "cannot open '" + dir + "': Is a directory");

    const std::string fifo = (d.path / "fifo").string();
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        CHECK(analyze_fs(fifo).diags.items().at(0).message ==
              "cannot open '" + fifo + "': not a regular file");
        const std::string main = d.write("main.mua", ".INCLUDE \"fifo\"\n");
        const Parsed r = analyze_fs(main);
        CHECK(r.errors().at(0) ==
              at(1, 10, "cannot open '" + (d.path / "fifo").string() + "': not a regular file"));
    }

    const std::string secret = d.write("secret.mua", ".CODE\nA: Hlt\n");
    if (::geteuid() != 0 && ::chmod(secret.c_str(), 0) == 0) {
        CHECK(analyze_fs(secret).diags.items().at(0).message ==
              "cannot open '" + secret + "': Permission denied");
    }
    const std::string missing = (d.path / "missing.mua").string();
    CHECK(analyze_fs(missing).diags.items().at(0).message ==
          "cannot open '" + missing + "': No such file or directory");
}

TEST_CASE("FsLoader: the size check happens before reading") {
    TempDir d;
    const std::string f = d.write("big.mua", std::string(1000, ';'));
    FsLoader loader;
    CHECK(loader.read(f, 999).error() == kSourceTooLarge);
    CHECK(loader.read(f, 1000).has_value());
    ParseLimits lim;
    lim.max_source_bytes = 999;
    Diagnostics diags;
    const Program p = parse_program(f, loader, diags, lim);
    CHECK(format_diagnostics(diags, p.files) ==
          f + ":1:1: error: source text exceeds the limit of 999 bytes\n");
}

TEST_CASE("includes: identity follows symlinks; a loop is one error") {
    TempDir d;
    d.write("main.mua", ".INCLUDE \"d/main.mua\"\n");
    fs::create_directory_symlink(".", d.path / "d");
    const std::string main = (d.path / "main.mua").string();
    const Parsed r = analyze_fs(main);
    REQUIRE(r.diags.items().size() == 1);
    // The path is shown as written.
    CHECK(r.errors()[0] == at(1, 10, "recursive include of '" + d.path.string() + "/d/main.mua'"));

    // A file symlink to the including file is the same file.
    TempDir e;
    e.write("a.mua", ".INCLUDE \"alias.mua\"\n");
    fs::create_symlink("a.mua", e.path / "alias.mua");
    const Parsed r2 = analyze_fs((e.path / "a.mua").string());
    REQUIRE(r2.diags.items().size() == 1);
    CHECK(r2.errors()[0].find("recursive include of") != std::string::npos);

    // Including the same real file twice through different names is allowed.
    TempDir g;
    g.write("lib.mua", ".CODE\nL: Hlt\n");
    fs::create_symlink("lib.mua", g.path / "lib2.mua");
    g.write("m.mua", ".INCLUDE \"lib.mua\"\n.INCLUDE \"lib2.mua\"\n");
    FsLoader loader;
    Diagnostics diags;
    const Program p = parse_program((g.path / "m.mua").string(), loader, diags);
    CHECK(!diags.has_errors());
    CHECK(p.blocks.size() == 2);
}

TEST_CASE("includes: display paths are as written") {
    const Files f{{"lib/a.mua", ".CODE\nA: Foo\n"}};
    const Parsed r = parse_src(".INCLUDE \"./lib/../lib/a.mua\"\n", f);
    CHECK(r.text() == "./lib/../lib/a.mua:2:4: error: expected a command name, found 'Foo'\n"
                      "t.mua:1:1: note: included from here\n");
}

TEST_CASE("diagnostics: the note of an included file carries its own include chain") {
    const Files f{{"lib.mua", ".CODE\nJ: Jmp\n"}};
    const Parsed r =
        analyze_src(".DATA\nENTRY: J\n.INCLUDE \"lib.mua\"\n.INCLUDE \"lib.mua\"\n", f);
    REQUIRE(!r.diags.items().empty());
    CHECK(format_diagnostic(r.diags.items()[0], r.program.files) ==
          "lib.mua:2:1: error: duplicate label 'J'\n"
          "lib.mua:2:1: note: first defined here\n"
          "t.mua:3:1: note: included from here\n"
          "t.mua:4:1: note: included from here\n");
}

// ---- diagnostics bookkeeping ----

TEST_CASE("diagnostics: only the first 1000 in source order are kept") {
    Diagnostics ds;
    for (std::uint32_t line = 1500; line >= 1; --line) {
        ds.error(SourceLoc{0, line, 1}, "e" + std::to_string(line));
    }
    const auto items = ds.items();
    REQUIRE(items.size() == 1000);
    CHECK(items.front().loc.line == 1);
    CHECK(items.back().loc.line == 1000);
    CHECK(ds.truncated());
    CHECK(ds.raised() == 1500);
    for (std::size_t i = 1; i < items.size(); ++i) {
        REQUIRE(items[i - 1].loc.line < items[i].loc.line);
    }
    const std::vector<SourceFile> files{{"f", std::nullopt}};
    CHECK(format_diagnostics(ds, files).ends_with(
        "too many errors; only the first 1000 are shown\n"));
}

TEST_CASE("diagnostics: a flood of lexical errors does not crowd out earlier errors") {
    std::string src = ".DATA\nA: )\n";
    for (int i = 0; i < 3000; ++i) {
        src += "%\n";
    }
    const Parsed r = parse_src(src);
    const auto errs = r.errors();
    REQUIRE(errs.size() == 1000);
    CHECK(errs[0] == at(2, 4, "expected a data word, found ')'"));
    CHECK(errs[1] == at(3, 1, "unexpected character '%'"));
    CHECK(r.diags.truncated());
}

TEST_CASE("diagnostics: the same message at the same place is reported once") {
    const Parsed r = analyze_src(".CODE\nJ: Jmp\n\n.DATA\nENTRY: J\nS: \"abcdef\", NOPE\n");
    CHECK(r.errors() == V{at(6, 14, "unknown label 'NOPE'")});
}

// ---- grammar details ----

TEST_CASE("2: a blank line ends an expression inside braces too") {
    const Parsed r = parse_src(".DATA\n{ ENTRY: ROT 5\n\n-3 }\n");
    REQUIRE(!r.diags.has_errors());
    CHECK(data_words(r.program) == V{"ROT", "0t12", "(neg 0t10)"});
    CHECK(parse_src(".DATA\n{ ENTRY: ROT 5\n\n+ 3 }\n").errors() ==
          V{at(4, 1, "expected a data word, found '+'")});
    CHECK(parse_src(".DATA\nENTRY: ROT 5\n\n+ 3\n").errors() ==
          V{at(4, 1, "expected a data word, found '+'")}); // as outside braces
    // Without a blank line the expression continues.
    CHECK(data_words(parse_src(".DATA\n{ ENTRY: ROT 5\n+ 3 }\n").program) ==
          V{"ROT", "(+ 0t12 0t10)"});
    // The same for a string's separator.
    const Parsed s = parse_src(".DATA\n{ S: \"ab\"\n\n, 7 }\n");
    CHECK(s.errors() == V{at(4, 1, "expected a data word, found ','")});
}

TEST_CASE("2: the line grouping of data words is kept and printed") {
    const Parsed r = parse_src(".DATA\n{\nENTRY: ROT 'H'<<1 R_ROT\n  OUT ?- R_OUT /* c\n */ X: 1\n"
                               "}\n");
    REQUIRE(!r.diags.has_errors());
    std::vector<bool> starts;
    for (const Cell& c : r.program.blocks[0].cells) {
        starts.push_back(c.starts_line);
    }
    CHECK(starts == std::vector<bool>{true, false, false, true, false, false, true});
    CHECK(print_program(r.program) == ".DATA\n{\n"
                                      "ENTRY:  ROT 'H'<<1 R_ROT\n"
                                      "        OUT ?- R_OUT\n"
                                      "X:      1\n"
                                      "}\n");
    // Different grouping, different structure.
    const Parsed one = parse_src(".DATA\nENTRY: A B\n");
    const Parsed two = parse_src(".DATA\nENTRY: A\nB\n");
    CHECK(!same_structure(one.program, two.program));
    // The first cell of a block always starts a line, even after `{`.
    CHECK(parse_src(".DATA\n{ A: 1 }\n").program.blocks[0].cells[0].starts_line);
}

TEST_CASE("2: grouping survives labels mid-line and parenthesised minus words") {
    const Parsed r = parse_src(".DATA\nA: 1 B: (-1) C: D: (-2)+3 (-4)\nE: 5\n");
    REQUIRE(!r.diags.has_errors());
    const std::string text = print_program(r.program);
    CHECK(text == ".DATA\nA:      1 B: (-1) C: D: (-2+3) (-4)\nE:      5\n");
    const Parsed again = parse_src(text);
    REQUIRE(!again.diags.has_errors());
    CHECK(same_structure(r.program, again.program));
}

TEST_CASE("8.1: .WIDTH after any section or .INCLUDE") {
    const Files f{{"lib.mua", ".CODE\nL: Hlt\n"}};
    const std::string msg = "'.WIDTH' must appear before any section or .INCLUDE";
    CHECK(parse_src(".INCLUDE \"lib.mua\"\n.WIDTH 12\n", f).errors() == V{at(2, 1, msg)});
    CHECK(parse_src(".DATA\n.WIDTH 12\n").errors() == V{at(2, 1, msg)});
    CHECK(!parse_src(".WIDTH 12\n.INCLUDE \"lib.mua\"\n", f).diags.has_errors());
}

TEST_CASE("8.2: @expr { starts a new block right after an unbraced block's last line") {
    const Parsed r = parse_src(".CODE\nA: Rot\n@5 { B: Hlt }\n");
    REQUIRE(!r.diags.has_errors());
    REQUIRE(r.program.blocks.size() == 2);
    CHECK(!r.program.blocks[0].braced);
    CHECK(!r.program.blocks[0].offset);
    CHECK(r.program.blocks[1].braced);
    REQUIRE(r.program.blocks[1].offset);
    CHECK(r.program.blocks[1].cells[0].labels[0].name == "B");
}

// ---- recovery polish ----

TEST_CASE("recovery: a section directive inside braces does not add an unmatched }") {
    CHECK(parse_src(".CODE\n{ A: Rot\n.DATA\nB: 1\n}\n").errors() ==
          V{at(3, 1, "'.DATA' inside braces; missing '}'")});
    // A second stray } is still reported.
    CHECK(parse_src(".CODE\n{ A: Rot\n.DATA\nB: 1\n}\n}\n").errors() ==
          V{at(3, 1, "'.DATA' inside braces; missing '}'"), at(6, 1, "unmatched '}'")});
}

TEST_CASE("recovery: a missing ':' after a label gets a hint") {
    CHECK(parse_src(".CODE\nROT Rot/Nop\n").errors() ==
          V{at(2, 1, "expected a command name, found 'ROT' (missing ':' after a label?)")});
    CHECK(parse_src(".DATA\nENTRY Rot\n").errors() ==
          V{at(2, 7,
               "'Rot' is a command and is only allowed in a .CODE block (missing ':' after a "
               "label?)")});
}

TEST_CASE("recovery: an unterminated include string does not open a file") {
    const Files f{{"lib.mua", ".CODE\nL: Hlt\n"}};
    CHECK(parse_src(".INCLUDE \"lib.mua\n", f).errors() ==
          V{at(1, 10, "unterminated string literal")});
    CHECK(parse_src(".INCLUDE \"nope.mua\n").errors() ==
          V{at(1, 10, "unterminated string literal")});
}

TEST_CASE("recovery: a { without a section does not skip across directives") {
    CHECK(parse_src("{ A: 1\n.DATA\nB: 2\n}\n").errors() ==
          V{at(1, 1, "block outside any section: expected .CODE or .DATA first"),
            at(4, 1, "unmatched '}'")});
}

TEST_CASE("recovery: an over-deep expression is one error") {
    const Parsed r = parse_src(".DATA\nA: " + std::string(5000, '(') + "1" +
                               std::string(5000, ')') + "\nB: 1\n");
    CHECK(r.errors().size() == 1);
    const Parsed m = parse_src(".DATA\nA: " + std::string(5000, '-') + "1\nB: 1\n");
    CHECK(m.errors().size() == 1);
}
