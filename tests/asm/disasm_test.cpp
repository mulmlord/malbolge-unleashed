// The disassembler (issue #17): every golden example disassembles, with and without its label map,
// to a .mua that re-assembles to the same program text; label maps round-trip; texts that are not
// from `malm asm` are listed cell by cell.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "asm/assemble.hpp"
#include "asm/disasm.hpp"
#include "asm/label_map.hpp"
#include "asm_test_util.hpp"
#include "core/placement.hpp"

using namespace asmtest;

namespace {

Assembly assemble_program(const Program& program, Diagnostics& diags,
                          const std::vector<SourceFile>& files) {
    auto a = assemble(program, diags);
    INFO(format_diagnostics(diags, files));
    REQUIRE(a.has_value());
    return std::move(*a);
}

Assembly assemble_example(const std::string& name) {
    FsLoader loader;
    Diagnostics diags;
    const Program p = analyze(std::string(MALM_EXAMPLES_DIR) + "/" + name, loader, diags);
    INFO(format_diagnostics(diags, p.files));
    REQUIRE(!diags.has_errors());
    return assemble_program(p, diags, p.files);
}

Assembly assemble_source(const std::string& src) {
    Parsed p = analyze_src(src);
    INFO(src);
    INFO(p.text());
    REQUIRE(!p.diags.has_errors());
    return assemble_program(p.program, p.diags, p.program.files);
}

std::vector<MapEntry> map_of(const Assembly& a) {
    auto m = parse_label_map(format_label_map(a.layout));
    REQUIRE(m.has_value());
    return std::move(*m);
}

Disassembly disasm(const std::vector<std::uint8_t>& text, const std::vector<MapEntry>* map) {
    DisasmOptions o;
    o.name = "t.malm";
    o.map = map;
    auto d = disassemble(text, o);
    INFO((d ? std::string() : d.error()));
    REQUIRE(d.has_value());
    return std::move(*d);
}

// Disassembles `a` with and without its map; both outputs must re-assemble to the same text.
void check_round_trip(const Assembly& a) {
    const std::vector<MapEntry> map = map_of(a);
    for (const bool with_map : {true, false}) {
        CAPTURE(with_map);
        const Disassembly d = disasm(a.startup.text, with_map ? &map : nullptr);
        CHECK(d.recognized);
        CHECK(d.warning == "");
        INFO(d.text);
        CHECK(assemble_source(d.text).startup.text == a.startup.text);
    }
}

} // namespace

TEST_CASE("every golden example re-assembles from its disassembly to the same text") {
    for (const char* f :
         {"hello.mua", "cat.mua", "halting_cat.mua", "digital_root.mua", "adder.mua"}) {
        CAPTURE(f);
        check_round_trip(assemble_example(f));
    }
}

TEST_CASE("cycles of every length, counters, U_ runs and head-1 blocks round-trip") {
    check_round_trip(assemble_source(R"(.CODE
C5:     Nop/Nop/Nop/Nop/MovD
        Jmp

C9:     Nop/Nop/Nop/Nop/Nop/Nop/Nop/Nop/MovD
        Jmp

ROT:    Rot/Nop
        Jmp

HALT:   Hlt

@1t2000000000
SLED:   RNop
        RNop
        Jmp

.DATA
ENTRY:  C5 X R_C5
        C9 X R_C9
        ROT 'A'<<1 R_ROT
        U_SLED Y 1 2
Y:      HALT
X:      HALT
        0t1 0t12 1t0
)"));
}

TEST_CASE("the output reads like the source") {
    const Assembly a = assemble_example("hello.mua");
    const std::vector<MapEntry> map = map_of(a);
    const std::string with_map = disasm(a.startup.text, &map).text;
    CHECK(with_map.find(".WIDTH ") != std::string::npos);
    CHECK(with_map.find("\nENTRY:          ROT 'H'<<1 R_ROT ") != std::string::npos);
    CHECK(with_map.find("\n                OUT ? R_OUT ") != std::string::npos);
    CHECK(with_map.find("\nROT:            Rot/Nop ") != std::string::npos);
    CHECK(with_map.find("p=59 'J' 2-cycle #1") != std::string::npos);
    const std::string without = disasm(a.startup.text, nullptr).text;
    CHECK(without.find("\nROT:            Rot/Nop ") != std::string::npos);
    CHECK(without.find("\nOUT:            Out/Nop ") != std::string::npos);
    CHECK(without.find("\nHALT:           Hlt") != std::string::npos);
    CHECK(without.find("code reached from ENTRY") != std::string::npos);

    const Assembly hc = assemble_example("halting_cat.mua");
    const std::vector<MapEntry> hc_map = map_of(hc);
    const std::string flags = disasm(hc.startup.text, &hc_map).text;
    CHECK(flags.find("\nEW1:            OPR W1: ...1 R_OPR ") != std::string::npos);
    CHECK(flags.find("\n                FO DO_OUT R_FO ") != std::string::npos);
    CHECK(flags.find("\n                MOVED LOOP ") != std::string::npos);
    CHECK(disasm(hc.startup.text, nullptr).text.find("\nFLAG:           Nop/MovD ") !=
          std::string::npos);
}

TEST_CASE("label maps round-trip through text") {
    const Assembly a = assemble_example("halting_cat.mua");
    const std::vector<MapEntry> map = map_of(a);
    REQUIRE(map.size() == a.layout.labels.size());
    for (const MapEntry& e : map) {
        CAPTURE(e.name);
        const LabelInfo& l = a.layout.labels.at(e.name);
        CHECK(e.addr == l.addr);
        CHECK(e.section == l.section);
        CHECK(e.first == a.layout.blocks[l.block].first);
        CHECK(e.last == a.layout.blocks[l.block].last);
    }
}

TEST_CASE("malformed label maps name the line") {
    for (const auto& [text, error] : std::vector<std::pair<std::string, std::string>>{
             {"A 0t1 code 0t1", "line 1: expected 'name address code|data first last'"},
             {"\nA 0t1 code 0t1 0t1 x", "line 2: expected"},
             {"1A 0t1 code 0t1 0t1", "line 1: '1A' is not a label name"},
             {"A 0t1 text 0t1 0t1", "line 1: expected 'code' or 'data', got 'text'"},
             {"A 0t3 code 0t1 0t1", "line 1: '0t3' is not an address"},
             {"A 0t1 data 0t10 0t11", "line 1: the address of 'A' is not inside its block"},
             {"A ...11 data 0t1 0t11", "line 1: the address of 'A' is not inside its block"}}) {
        CAPTURE(text);
        const auto m = parse_label_map(text);
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error().starts_with(error));
    }
    const auto ok = parse_label_map("\n  A 0t1  code 0t1 0t10\r\n\nB 0t11 data 0t11 0t11");
    REQUIRE(ok.has_value());
    CHECK(ok->size() == 2);
}

TEST_CASE("a label map of another program is an error") {
    const Assembly hello = assemble_example("hello.mua");
    const Assembly cat = assemble_example("cat.mua");
    const std::vector<MapEntry> map = map_of(hello);
    DisasmOptions o;
    o.map = &map;
    const auto d = disassemble(cat.startup.text, o);
    REQUIRE_FALSE(d.has_value());
    CHECK(d.error().starts_with("the label map does not match this program: "));
}

TEST_CASE("texts not from malm asm are listed cell by cell") {
    using malm::core::loadable_char;
    // Out, then Hlt: I/O before the startup code's final Jmp.
    const std::vector<std::uint8_t> text{loadable_char(0, Op::Out), loadable_char(1, Op::Hlt)};
    DisasmOptions o;
    o.name = "p.mal";
    auto d = disassemble(text, o);
    REQUIRE(d.has_value());
    CHECK_FALSE(d->recognized);
    CHECK(d->text.starts_with("; malm disasm: p.mal: 2 cells, unleashed\n; not a program from "
                              "malm asm: it executes Out at step 1, before a second Jmp: listing "
                              "the program text\n"));
    CHECK(d->text.find("\n0t0             0    ") != std::string::npos);
    CHECK(d->text.find(" Out   ") != std::string::npos);
    CHECK(d->text.find("\n0t1             1    ") != std::string::npos);

    // A map needs a recognised program.
    const std::vector<MapEntry> none;
    o.map = &none;
    CHECK_FALSE(disassemble(text, o).has_value());

    // --classic and --raw always list; the listing of an assembled program starts with the Jmp.
    o.map = nullptr;
    o.classic = true;
    CHECK(disassemble(text, o)->text.find("; a classic program: listing") != std::string::npos);
    o.classic = false;
    o.raw = true;
    const Assembly a = assemble_example("cat.mua");
    d = disassemble(a.startup.text, o);
    REQUIRE(d.has_value());
    CHECK_FALSE(d->recognized);
    CHECK(d->text.find("\n0t0             0    b    i    Jmp   68-cycle") != std::string::npos);
    std::size_t lines = 0;
    for (const char c : d->text) {
        lines += c == '\n' ? 1 : 0;
    }
    CHECK(lines == 3 + a.startup.text.size());
}
