#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rv/machine.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;
using malm::rv::Image;
using malm::rv::Machine;
using malm::rv::Stop;

namespace {

constexpr u32 kRam = 0x10000;
constexpr u32 kCode = 0x1000;
constexpr u32 kBuf = 0x8000; // scratch data area of the test programs

using Code = std::vector<u32>;

Code operator+(Code a, const Code& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// lui + addi, with the carry from addi's sign extension folded into the upper part.
Code load_const(u32 rd, u32 v) {
    return {lui(rd, (v + 0x800) >> 12 & 0xfffff),
            addi(rd, rd, static_cast<std::int32_t>(v << 20) >> 20)};
}

Code sys(u32 n, u32 x10 = 0, u32 x11 = 0, u32 x12 = 0) {
    return load_const(a0, x10) + load_const(a1, x11) + load_const(a2, x12) + load_const(a7, n) +
           Code{kEcall};
}

const Code kExit0 = sys(93);

Image image_of(const Code& code) {
    std::vector<std::uint8_t> bytes;
    for (const u32 w : code) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    const u32 size = static_cast<u32>(bytes.size());
    return {kCode, {{kCode, size, std::move(bytes), true}}, kCode + size};
}

struct Run {
    Machine m;
    Stop stop;
    std::string out;
};

Run run(const Code& code, std::string input = "") {
    Machine m(image_of(code), kRam);
    StringIo io;
    io.in = std::move(input);
    Stop stop = m.run(io);
    return {std::move(m), std::move(stop), io.out};
}

// Runs a program that must exit and returns the machine at the exit.
Machine exits(const Code& code) {
    Run r = run(code);
    INFO(r.stop.message);
    REQUIRE(r.stop.kind == Stop::Kind::Exit);
    return std::move(r.m);
}

// A single instruction at the entry point must fault with `message`, changing nothing.
void faults(const Code& setup, u32 insn, const std::string& message) {
    Machine m(image_of(setup + Code{insn}), kRam);
    StringIo io;
    for (std::size_t i = 0; i < setup.size(); ++i) {
        REQUIRE_FALSE(m.step(io));
    }
    const auto regs = m.regs();
    const u32 pc = m.pc();
    const auto stop = m.step(io);
    REQUIRE(stop);
    CHECK(stop->kind == Stop::Kind::Fault);
    CHECK(stop->message == message);
    CHECK(m.pc() == pc);
    CHECK(m.regs() == regs);
    CHECK(m.steps() == setup.size());
}

} // namespace

TEST_CASE("machine: initial state") {
    Image image = image_of(kExit0);
    image.segments.push_back({0x4000, 0x20, {0xaa, 0xbb}, false});
    image.brk = 0x4020;
    Machine m(image, kRam);
    CHECK(m.pc() == kCode);
    CHECK(m.reg(2) == kRam);
    for (unsigned i = 0; i < 32; ++i) {
        CHECK((i == 2 || m.reg(i) == 0));
    }
    CHECK(m.brk() == 0x4020);
    REQUIRE(m.ram().size() == kRam);
    CHECK(m.peek32(kCode) == kExit0[0]);
    CHECK(m.peek32(0x4000) == 0xbbaau);
    CHECK(m.peek32(0x4004) == 0u);
    CHECK(m.peek32(kRam - 4) == 0u);
    CHECK_FALSE(m.peek32(kRam - 3));
    CHECK(m.steps() == 0);
}

TEST_CASE("machine: register-register operations, RV32I and RV32M") {
    struct Case {
        const char* name;
        u32 f3, f7, a, b, want;
    };
    constexpr u32 kMin = 0x80000000;
    const Case cases[] = {
        {"add wraps", 0, 0x00, 0x7fffffff, 1, kMin},
        {"sub", 0, 0x20, 0, 1, 0xffffffff},
        {"sll uses rs2[4:0]", 1, 0x00, 1, 33, 2},
        {"slt signed", 2, 0x00, 0xffffffff, 1, 1},
        {"sltu unsigned", 3, 0x00, 0xffffffff, 1, 0},
        {"xor", 4, 0x00, 0xf0f0f0f0, 0xff00ff00, 0x0ff00ff0},
        {"srl", 5, 0x00, kMin, 31, 1},
        {"sra", 5, 0x20, kMin, 31, 0xffffffff},
        {"sra by 32 is by 0", 5, 0x20, kMin, 32, kMin},
        {"or", 6, 0x00, 0xf0f0f0f0, 0x0f, 0xf0f0f0ff},
        {"and", 7, 0x00, 0xf0f0f0f0, 0xff, 0xf0},
        {"mul low word", 0, 0x01, 0xffffffff, 0xffffffff, 1},
        {"mulh min*min", 1, 0x01, kMin, kMin, 0x40000000},
        {"mulh -1*1", 1, 0x01, 0xffffffff, 1, 0xffffffff},
        {"mulhsu -1*max", 2, 0x01, 0xffffffff, 0xffffffff, 0xffffffff},
        {"mulhsu min*max", 2, 0x01, kMin, 0xffffffff, kMin},
        {"mulhu max*max", 3, 0x01, 0xffffffff, 0xffffffff, 0xfffffffe},
        {"div truncates", 4, 0x01, 7, 0xfffffffe, 0xfffffffd},
        {"div by zero", 4, 0x01, 7, 0, 0xffffffff},
        {"div overflow", 4, 0x01, kMin, 0xffffffff, kMin},
        {"divu", 5, 0x01, 0xffffffff, 2, 0x7fffffff},
        {"divu by zero", 5, 0x01, 7, 0, 0xffffffff},
        {"rem sign of dividend", 6, 0x01, 0xfffffff9, 2, 0xffffffff},
        {"rem by zero", 6, 0x01, 0xfffffff9, 0, 0xfffffff9},
        {"rem overflow", 6, 0x01, kMin, 0xffffffff, 0},
        {"remu", 7, 0x01, 0xffffffff, 10, 5},
        {"remu by zero", 7, 0x01, 7, 0, 7},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const Machine m = exits(load_const(t0, c.a) + load_const(t1, c.b) +
                                Code{op(c.f3, c.f7, t2, t0, t1)} + kExit0);
        CHECK(m.reg(t2) == c.want);
    }
}

TEST_CASE("machine: immediate operations, lui and auipc") {
    const Machine m = exits(Code{
                                li(t0, -5),                    // 0x1000
                                i_type(0x13, 18, 2, t0, -4),   // slti s2 = -5 < -4
                                i_type(0x13, 19, 3, t0, -1),   // sltiu s3 = 0xfffffffb < 0xffffffff
                                i_type(0x13, 20, 4, t0, -1),   // xori s4 = ~t0
                                i_type(0x13, 21, 6, 0, 0x7ff), // ori s5
                                i_type(0x13, 22, 7, t0, 0xf0), // andi s6
                                i_type(0x13, 23, 1, t0, 4),    // slli s7
                                i_type(0x13, 24, 5, t0, 1),    // srli s8
                                i_type(0x13, 25, 5, t0, 0x401), // srai s9
                                lui(26, 0xfffff),               // s10
                                auipc(27, 1),                   // s11 = 0x1028 + 0x1000
                            } +
                            kExit0);
    CHECK(m.reg(t0) == 0xfffffffb);
    CHECK(m.reg(18) == 1);
    CHECK(m.reg(19) == 1);
    CHECK(m.reg(20) == 4);
    CHECK(m.reg(21) == 0x7ff);
    CHECK(m.reg(22) == 0xf0);
    CHECK(m.reg(23) == 0xffffffb0);
    CHECK(m.reg(24) == 0x7ffffffd);
    CHECK(m.reg(25) == 0xfffffffd);
    CHECK(m.reg(26) == 0xfffff000);
    CHECK(m.reg(27) == 0x2028);
}

TEST_CASE("machine: x0 is hardwired to zero") {
    const Machine m = exits(Code{li(0, 5), lui(0, 1), op(0, 0, t0, 0, 0)} + kExit0);
    CHECK(m.reg(0) == 0);
    CHECK(m.reg(t0) == 0);
}

TEST_CASE("machine: loads and stores, little-endian and misaligned") {
    const Machine m = exits(load_const(t0, kBuf) + load_const(t1, 0x80ff7f01) +
                            Code{
                                store(2, t0, t1, 0), // sw  01 7f ff 80
                                load(0, 18, t0, 0),  // lb  s2
                                load(0, 19, t0, 2),  // lb  s3
                                load(4, 20, t0, 2),  // lbu s4
                                load(1, 21, t0, 2),  // lh  s5
                                load(5, 22, t0, 2),  // lhu s6
                                load(2, 23, t0, 1),  // lw  s7, misaligned
                                store(0, t0, t1, 5), // sb  01 at +5
                                store(1, t0, t1, 7), // sh  01 7f at +7, misaligned
                                load(2, 24, t0, 4),  // lw  s8: 00 01 00 01
                            } +
                            kExit0);
    CHECK(m.reg(18) == 0x01);
    CHECK(m.reg(19) == 0xffffffff);
    CHECK(m.reg(20) == 0xff);
    CHECK(m.reg(21) == 0xffff80ff);
    CHECK(m.reg(22) == 0x80ff);
    CHECK(m.reg(23) == 0x0080ff7f);
    CHECK(m.peek32(kBuf + 4) == 0x01000100u);
    CHECK(m.peek32(kBuf + 8) == 0x0000007fu);
    CHECK(m.reg(24) == 0x01000100);
}

TEST_CASE("machine: jumps and branches") {
    // jal over one instruction; jalr to an odd address clears bit 0; jalr with rd == rs1 uses
    // the old rs1.
    const Machine m = exits(Code{
                                jal(1, 8),                 // 0x1000 -> 0x1008, ra = 0x1004
                                li(18, 1),                 // 0x1004 skipped
                                load_const(t0, 0x1015)[0], // 0x1008 lui t0
                                load_const(t0, 0x1015)[1], // 0x100c addi t0 = 0x1015
                                jalr(t0, t0, 0),           // 0x1010 -> 0x1014, t0 = 0x1014
                                li(19, 3),                 // 0x1014 s3 = loop counter
                                addi(19, 19, -1),          // 0x1018 loop:
                                addi(20, 20, 1),           // 0x101c   s4++
                                b_type(1, 19, 0, -8),      // 0x1020   bne s3, x0, loop
                                b_type(4, 0, 19, 8),       // 0x1024 blt x0, s3: not taken
                                b_type(6, 19, 1, 8),       // 0x1028 bltu 0 < ra: taken
                                li(18, 1),                 // 0x102c skipped
                                b_type(5, 0, 19, 8),       // 0x1030 bge 0 >= 0: taken
                                li(18, 1),                 // 0x1034 skipped
                                b_type(7, 19, 1, 8),       // 0x1038 bgeu 0 >= ra: not taken
                                b_type(0, 0, 19, 8),       // 0x103c beq: taken
                                li(18, 1),                 // 0x1040 skipped
                            } +
                            kExit0);
    CHECK(m.reg(1) == 0x1004);
    CHECK(m.reg(t0) == 0x1014);
    CHECK(m.reg(18) == 0);
    CHECK(m.reg(19) == 0);
    CHECK(m.reg(20) == 3);
}

TEST_CASE("machine: fence and fence.i are no-ops, and fetch sees stores") {
    // Overwrite the instruction after fence.i with `li s2, 7`, then run it.
    const Machine m = exits(load_const(t0, li(18, 7)) +
                            Code{
                                auipc(t1, 0),         // t1 = this pc
                                store(2, t1, t0, 16), // [pc + 16] = li s2, 7
                                0x0ff0000f,           // fence iorw, iorw
                                0x0000100f,           // fence.i
                                li(18, 1),            // replaced
                            } +
                            kExit0);
    CHECK(m.reg(18) == 7);
}

TEST_CASE("machine: illegal instructions fault without side effects") {
    const Code none;
    faults(none, 0x00000000, "illegal instruction 0x00000000");
    faults(none, 0xffffffff, "illegal instruction 0xffffffff");
    faults(none, 0x00000001, "illegal instruction 0x00000001");                    // c.nop: no RVC
    faults(none, i_type(0x13, t0, 1, t0, 0x20), "illegal instruction 0x02029293"); // slli, shamt 32
    faults(none, i_type(0x13, t0, 5, t0, 0x201), "illegal instruction 0x2012d293"); // srai f7 0x10
    faults(none, op(0, 0x02, t0, t0, t0), "illegal instruction 0x045282b3");
    faults(none, op(1, 0x20, t0, t0, t0), "illegal instruction 0x405292b3"); // sll with f7 0x20
    faults(none, load(3, t0, 0, 0), "illegal instruction 0x00003283");       // ld
    faults(none, load(6, t0, 0, 0), "illegal instruction 0x00006283");       // lwu
    faults(none, store(3, 0, t0, 0), "illegal instruction 0x00503023");      // sd
    faults(none, b_type(2, 0, 0, 8), "illegal instruction 0x00002463");
    faults(none, i_type(0x67, t0, 1, 0, 0), "illegal instruction 0x000012e7");     // jalr f3 1
    faults(none, 0x0000200f, "illegal instruction 0x0000200f");                    // fence f3 2
    faults(none, i_type(0x73, t0, 1, 0, 0x300), "illegal instruction 0x300012f3"); // csrrw
    faults(none, 0x000000f3, "illegal instruction 0x000000f3"); // ecall encoding with rd != 0
    faults(none, kEbreak, "ebreak");
}

TEST_CASE("machine: misaligned jump targets and accesses outside RAM fault") {
    const Code none;
    faults(none, jal(1, 2), "misaligned jump target 0x00001002");
    faults(none, b_type(0, 0, 0, 6), "misaligned jump target 0x00001006");
    faults(Code{li(t0, 0x102)}, jalr(1, t0, 0), "misaligned jump target 0x00000102");
    faults(Code{lui(t0, 0x10)}, load(2, t1, t0, -2), "load from 0x0000fffe outside guest RAM");
    faults(Code{lui(t0, 0x10)}, load(0, t1, t0, 0), "load from 0x00010000 outside guest RAM");
    faults(none, store(1, 0, t0, -1), "store to 0xffffffff outside guest RAM");

    // A branch that is not taken does not check its target.
    const Machine m = exits(Code{b_type(1, 0, 0, 6)} + kExit0);
    CHECK(m.steps() == kExit0.size() + 1);

    // Jumping out of RAM faults at the fetch, with pc at the target.
    Run r = run(Code{lui(t0, 0x10), jalr(0, t0, 0)});
    CHECK(r.stop.kind == Stop::Kind::Fault);
    CHECK(r.stop.message == "instruction fetch outside guest RAM");
    CHECK(r.m.pc() == kRam);
    CHECK(r.m.steps() == 2);
}

TEST_CASE("machine: write and read") {
    const u32 s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23, s8 = 24, s9 = 25, s10 = 26,
              s11 = 27;
    const auto keep = [](u32 rd) { return Code{addi(rd, a0, 0)}; };
    const Code prog = load_const(t1, kBuf) + load_const(t0, 0x0a216968) +
                      Code{store(2, t1, t0, 0)} +           // "hi!\n"
                      sys(64, 1, kBuf, 3) + keep(s2) +      // write(1, "hi!", 3) = 3
                      sys(64, 2, kBuf + 3, 1) + keep(s3) +  // write(2, "\n", 1) = 1
                      sys(64, 0, kBuf, 1) + keep(s4) +      // write(0, ...) = -EBADF
                      sys(64, 1, kRam - 1, 2) + keep(s5) +  // buffer past the end of RAM: -EFAULT
                      sys(63, 0, kBuf, 4) + keep(s6) +      // read(0, buf, 4) = 4, "abcd"
                      sys(63, 0, kBuf + 4, 4) + keep(s7) +  // read(0, buf + 4, 4) = 2, "ef" (EOF)
                      sys(63, 0, kBuf, 4) + keep(s8) +      // at EOF: 0
                      sys(63, 1, kBuf, 4) + keep(s9) +      // read(1, ...) = -EBADF
                      sys(63, 0, kRam - 2, 4) + keep(s10) + // -EFAULT, nothing read
                      sys(64, 1, kBuf, 0) + keep(s11) +     // write of 0 bytes = 0
                      kExit0;
    const Run r = run(prog, "abcdef");
    REQUIRE(r.stop.kind == Stop::Kind::Exit);
    CHECK(r.out == "hi!\n");
    CHECK(r.m.reg(s2) == 3);
    CHECK(r.m.reg(s3) == 1);
    CHECK(r.m.reg(s4) == malm::rv::kEbadf);
    CHECK(r.m.reg(s5) == malm::rv::kEfault);
    CHECK(r.m.reg(s6) == 4);
    CHECK(r.m.reg(s7) == 2);
    CHECK(r.m.reg(s8) == 0);
    CHECK(r.m.reg(s9) == malm::rv::kEbadf);
    CHECK(r.m.reg(s10) == malm::rv::kEfault);
    CHECK(r.m.reg(s11) == 0);
    CHECK(r.m.peek32(kBuf) == 0x64636261u);
    CHECK(r.m.peek32(kBuf + 4) == 0x00006665u);
}

TEST_CASE("machine: brk, unknown syscalls and exit") {
    const u32 s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23;
    const auto keep = [](u32 rd) { return Code{addi(rd, a0, 0)}; };
    // The image ends at kCode + size; its exact value is read back with brk(0).
    const Code prog = sys(214, 0) + keep(s2) + // brk(0) = initial break
                      Code{addi(a0, s2, 0x100), li(a7, 214), kEcall} + keep(s3) + // grow
                      Code{addi(a0, s2, -4), li(a7, 214), kEcall} + keep(s4) +    // below: refused
                      sys(214, kRam + 4) + keep(s5) + // past RAM: refused
                      sys(214, kRam) + keep(s6) +     // up to the end of RAM
                      sys(1234) + keep(s7) +          // unknown: -ENOSYS
                      sys(93, 0x1ff);                 // exit(0x1ff): status 0xff
    const Run r = run(prog);
    REQUIRE(r.stop.kind == Stop::Kind::Exit);
    CHECK(r.stop.exit_code == 0xff);
    const u32 initial = kCode + static_cast<u32>(prog.size() * 4);
    CHECK(r.m.reg(s2) == initial);
    CHECK(r.m.reg(s3) == initial + 0x100);
    CHECK(r.m.reg(s4) == initial + 0x100);
    CHECK(r.m.reg(s5) == initial + 0x100);
    CHECK(r.m.reg(s6) == kRam);
    CHECK(r.m.reg(s7) == malm::rv::kEnosys);
    CHECK(r.m.brk() == kRam);
    // The exit ecall counts as a step and leaves pc on itself.
    CHECK(r.m.steps() == prog.size());
    CHECK(r.m.pc() == kCode + static_cast<u32>(prog.size() * 4) - 4);
}

TEST_CASE("machine: the register dump") {
    const Run r = run(sys(255, 0x80000001, 0xabc) + kExit0);
    REQUIRE(r.stop.kind == Stop::Kind::Exit);
    std::string want = "== registers ==\npc 0x00001020\n"; // the ecall, after 8 words
    for (unsigned i = 1; i < 32; ++i) {
        const std::map<unsigned, std::string> set = {
            {2, "00010000"}, {10, "80000001"}, {11, "00000abc"}, {17, "000000ff"}};
        want += "x" + std::to_string(i) + " 0x" + (set.contains(i) ? set.at(i) : "00000000") + "\n";
    }
    CHECK(r.out == want);
    CHECK(r.m.reg(a0) == 0); // exit's: the dump changed nothing, then exit set a0 to 0
}

TEST_CASE("machine: host I/O errors fault with E_IO") {
    Machine w(image_of(sys(64, 1, kCode, 4) + kExit0), kRam);
    StringIo io;
    io.write_error = true;
    Stop stop = w.run(io);
    CHECK(stop.kind == Stop::Kind::Fault);
    CHECK(stop.message == "E_IO: cannot write to stdout");
    CHECK(w.pc() == kCode + 32); // the write ecall

    Machine rd(image_of(sys(63, 0, kBuf, 4) + kExit0), kRam);
    StringIo bad_in;
    bad_in.in = "x";
    bad_in.read_error = true;
    stop = rd.run(bad_in);
    CHECK(stop.kind == Stop::Kind::Fault);
    CHECK(stop.message == "E_IO: cannot read from stdin");

    // An exit whose final flush fails is an E_IO fault too.
    Machine ex(image_of(kExit0), kRam);
    stop = ex.run(io);
    CHECK(stop.kind == Stop::Kind::Fault);
    CHECK(stop.message == "E_IO: cannot write to stdout");
}
