// Laid-out units run on the real VM (issue #14): every unit kind of docs/asm.md 3.2-3.3, 5.2 and 6
// does what the spec says, single-stepped where the intermediate state matters. The image is
// loaded as the startup code (#15) will leave it (layout_test_util.hpp).

#include <doctest/doctest.h>

#include <string>

#include "asm/cycles.hpp"
#include "core/ops.hpp"
#include "layout_test_util.hpp"

using namespace asmtest;
using malm::core::Status;
using malm::core::Vm;

namespace {

Value mem(const Vm& vm, Value a) { return vm.memory().read(a); }

std::uint8_t char_in(const Vm& vm, Value a) { return static_cast<std::uint8_t>(mem(vm, a).raw()); }

void step(Vm& vm, TestIo& io, Status want = Status::Continue) {
    const auto r = vm.step(io);
    if (r.fault) {
        INFO(malm::core::format_fault(*r.fault));
        REQUIRE(false);
    }
    REQUIRE(r.status == want);
}

std::string run(const Layout& l, std::string input = "", std::uint64_t max_steps = 100000) {
    Vm vm = load_image(l);
    TestIo io;
    io.in = std::move(input);
    const auto r = vm.run(io, max_steps);
    if (r.fault) {
        INFO(malm::core::format_fault(*r.fault));
        REQUIRE(false);
    }
    return io.out;
}

} // namespace

TEST_CASE("10.1: hello world") {
    std::string src = ".CODE\n"
                      "ROT:    Rot/Nop\n"
                      "        Jmp\n"
                      "\n"
                      "OUT:    Out/Nop\n"
                      "        Jmp\n"
                      "\n"
                      "HALT:   Hlt\n"
                      "\n"
                      ".DATA\n"
                      "{\n"
                      "ENTRY:";
    for (const char c : std::string("Hello, world!\n")) {
        src += std::string("  ROT ") + (c == '\n' ? "'\\n'" : "'" + std::string(1, c) + "'") +
               "<<1 R_ROT\n        OUT ?- R_OUT\n";
    }
    src += "        HALT\n}\n";
    const Layout l = laid(src);
    Vm vm = load_image(l);
    TestIo io;
    const auto r = vm.run(io, 10000);
    CHECK(r.kind == malm::core::RunResult::Kind::Halted);
    CHECK(io.out == "Hello, world!\n");
}

TEST_CASE("10.2: cat (endless): In, Out, and a MovD goto with a landing pad") {
    const Layout l = laid(".CODE\n"
                          "IN:     In/Nop\n"
                          "        Jmp\n"
                          "\n"
                          "OUT:    Out/Nop\n"
                          "        Jmp\n"
                          "\n"
                          "MOVED:  MovD/Nop\n"
                          "        Jmp\n"
                          "\n"
                          ".DATA\n"
                          "{\n"
                          "ENTRY:  R_MOVED\n"
                          "LOOP:   R_MOVED\n"
                          "        IN ?- R_IN\n"
                          "        OUT ?- R_OUT\n"
                          "        MOVED LOOP\n"
                          "}\n");
    const std::string out = run(l, "hi", 200);
    REQUIRE(out.size() >= 4);
    CHECK(out.substr(0, 4) == "hi\xA8\xA8"); // after EOF: ...2 mod 256 = 168
}

TEST_CASE("3.2: an X/Nop unit, single-stepped: jump, instruction, restore") {
    for (const char* op : {"Opr", "Rot"}) {
        CAPTURE(op);
        const Layout l = laid(std::string(".CODE\nX: ") + op +
                              "/Nop\n   Jmp\n\nHALT: Hlt\n\n.DATA\nENTRY: X 'q' R_X\n HALT\n");
        const Value x = addr_of(l, "X");
        const Value entry = addr_of(l, "ENTRY");
        const Value reserved = plus(x, -1);
        const std::uint8_t x0 = char_at(l, x);
        const std::uint8_t r0 = char_at(l, reserved);
        Vm vm = load_image(l);
        TestIo io;

        // 1. The Jmp at cell 0 reads [ENTRY] = X - 1, encrypts the reserved cell, resumes at X.
        step(vm, io);
        CHECK(vm.c() == x);
        CHECK(vm.d() == plus(entry, 1));
        CHECK(char_in(vm, reserved) == malm::core::encrypt(r0));
        // 2. The instruction runs on [D] = 'q' and its cell flips to Nop.
        const Value a0 = vm.a();
        step(vm, io);
        const Value want =
            op[0] == 'O' ? malm::core::crz(a0, nat('q')) : malm::core::rotr(nat('q'), l.width);
        CHECK(vm.a() == want);
        CHECK(mem(vm, plus(entry, 1)) == want);
        CHECK(char_in(vm, x) == cycle_next(x0));
        CHECK(op_at(x.mod94(), char_in(vm, x), 0) == Op::Nop);
        // 3. X's Jmp reads R_X: encrypting X restores it, and execution goes on at X + 1.
        step(vm, io);
        CHECK(char_in(vm, x) == x0);
        CHECK(vm.c() == plus(x, 1));
        CHECK(vm.d() == plus(entry, 3));
        // 4. The Jmp reads HALT - 1; 5. Hlt.
        step(vm, io);
        CHECK(vm.c() == addr_of(l, "HALT"));
        step(vm, io, Status::Halted);
    }
}

TEST_CASE("3.2: a bare Jmp continues with the next data word") {
    const Layout l = laid(".CODE\nNOP: Jmp\n\nHALT: Hlt\n\n.DATA\nENTRY: NOP HALT\n");
    Vm vm = load_image(l);
    TestIo io;
    step(vm, io);
    CHECK(vm.c() == addr_of(l, "NOP"));
    step(vm, io);
    CHECK(vm.c() == addr_of(l, "HALT"));
    step(vm, io, Status::Halted);
}

TEST_CASE("3.3: a flag: R_F sets it, `F target R_F` branches when set and falls through when "
          "clear") {
    const Layout l = laid(".CODE\n"
                          "F: Nop/MovD\n   Jmp\n\n"
                          "ROT: Rot/Nop\n   Jmp\n\n"
                          "OUT: Out/Nop\n   Jmp\n\n"
                          "HALT: Hlt\n\n"
                          ".DATA\n"
                          "ENTRY: ROT 'a'<<1 R_ROT\n"
                          "       F SET R_F\n"    // clear: fall through
                          "       OUT ?- R_OUT\n" // a
                          "       R_F\n"          // set
                          "       F SET R_F\n"    // set: branch, the flag clears itself
                          "       HALT\n"
                          "SET:   ROT 'b'<<1 R_ROT\n"
                          "       OUT ?- R_OUT\n" // b
                          "       F SET2 R_F\n"   // clear again: fall through
                          "       OUT ?- R_OUT\n" // b
                          "       HALT\n"
                          "SET2:  HALT\n");
    CHECK(run(l) == "abb");
}

TEST_CASE("3.3: a flag toggled k times through R_F is back in its first state after its cycle "
          "length") {
    for (const char* cycle : {"Nop/MovD", "Nop/MovD/Nop/Nop", "Nop/Nop/Nop/Nop/MovD",
                              "Nop/Nop/Nop/Nop/Nop/MovD", "Nop/Nop/Nop/Nop/Nop/Nop/Nop/Nop/MovD"}) {
        CAPTURE(cycle);
        const std::size_t len = (std::string(cycle).size() + 1) / 4; // "Nop/" per state
        std::string data;
        for (std::size_t i = 0; i < len; ++i) {
            data += " R_F";
        }
        const Layout l = laid(std::string(".CODE\nF: ") + cycle +
                              "\n   Jmp\n\nHALT: Hlt\n\n.DATA\nENTRY:" + data + " HALT\n");
        const Value f = addr_of(l, "F");
        const std::uint8_t f0 = char_at(l, f);
        CHECK(cycle_length(f0) == len);
        Vm vm = load_image(l);
        TestIo io;
        for (std::size_t k = 1; k <= len; ++k) {
            CAPTURE(k);
            step(vm, io); // a Jmp reads R_F: F advances one state, execution resumes at F + 1
            CHECK(vm.c() == plus(f, 1));
            CHECK(char_in(vm, f) == cycle_next(f0, k));
            CHECK((char_in(vm, f) == f0) == (k == len));
        }
        step(vm, io);
        step(vm, io, Status::Halted);
    }
}

TEST_CASE("3.3: a counter branches on every k-th pass") {
    const Layout l = laid(".CODE\n"
                          "CNT: Nop/Nop/Nop/Nop/MovD\n     Jmp\n\n"
                          "ROT: Rot/Nop\n     Jmp\n\n"
                          "OUT: Out/Nop\n     Jmp\n\n"
                          "MOVED: MovD/Nop\n     Jmp\n\n"
                          "HALT: Hlt\n\n"
                          ".DATA\n"
                          "ENTRY: ROT 'a'<<1 R_ROT\n"
                          "       R_MOVED\n"
                          "LOOP:  R_MOVED\n"
                          "       CNT DONE\n"
                          "       OUT ?- R_OUT\n"
                          "       MOVED LOOP\n"
                          "DONE:  HALT\n");
    CHECK(run(l) == "aaaa");
}

TEST_CASE("5.2: U_X Y: the jump lands on the RNop run and X operates on Y") {
    const Layout l = laid(".CODE\n"
                          "ROT: Rot/Nop\n     Jmp\n\n"
                          "OUT: Out/Nop\n     Jmp\n\n"
                          "HALT: Hlt\n\n"
                          ".DATA\n"
                          "ENTRY: U_ROT VAR ?- ?-\n"
                          "VAR:   'H'<<1\n"
                          "       R_ROT\n"
                          "       OUT ?- R_OUT\n"
                          "       HALT\n");
    const Value rot = addr_of(l, "ROT");
    Vm vm = load_image(l);
    TestIo io;
    step(vm, io);
    CHECK(vm.c() == plus(rot, -2)); // the first RNop of the run
    step(vm, io);
    step(vm, io);
    CHECK(vm.c() == rot);
    CHECK(vm.d() == addr_of(l, "VAR"));
    const auto r = vm.run(io, 1000);
    CHECK(r.kind == malm::core::RunResult::Kind::Halted);
    CHECK(io.out == "H");
}

TEST_CASE("6: a value branch through the @C21 sled executes 0, 1 or 2 RNops") {
    const char* const values[] = {"C2", "C21", "C20"};
    for (int k = 0; k < 3; ++k) {
        CAPTURE(values[k]);
        const Layout l = laid(std::string(".CODE\n"
                                          "MOVED: MovD/Nop\n       Jmp\n\n"
                                          "H0: Hlt\n\nH1: Hlt\n\nH2: Hlt\n\n"
                                          "@C21\n"
                                          "SLED: RNop\n      RNop\n\n"
                                          ".DATA\n"
                                          "ENTRY: MOVED VAR\n"
                                          "VAR:   ") +
                              values[k] + " H0 H1 H2\n");
        Vm vm = load_image(l);
        TestIo io;
        const auto r = vm.run(io, 100);
        REQUIRE(r.kind == malm::core::RunResult::Kind::Halted);
        CHECK(vm.c() == addr_of(l, "H" + std::to_string(k)));
        CHECK(vm.steps() == 5 + static_cast<std::uint64_t>(k)); // Jmp MovD Jmp, k RNops, Jmp, Hlt
    }
}
