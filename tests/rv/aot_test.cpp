// rv-aot (issue #29): the control-flow graph it recovers, and translated programs against the C++
// reference emulator: their output, exit status, stop code, final registers and RAM.
//
// Each program is compiled once and run in the VM per input; the reference emulator runs the same
// ELF. A debug build has sanitizers, and its assembler and VM are ~70x slower, so the programs
// there are smaller or slow (nightly).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../hla/hla_test_util.hpp"
#include "aot/aot.hpp"
#include "aot/cfg.hpp"
#include "rv/machine.hpp"
#include "rv/semantics.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;
using malm::rv::sem::StopCode;

namespace {

// The literal suite name is what malm_add_test looks for to register the slow cases.
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
const auto kSlowInDebug = doctest::test_suite("");
#else
constexpr bool kDebugBuild = true;
const auto kSlowInDebug = doctest::test_suite("slow");
#endif

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

// Register numbers besides rv_test_util's.
constexpr u32 ra = 1, s0 = 8, s1 = 9, s2 = 18, s3 = 19, s4 = 20, a3 = 13;

constexpr u32 kText = 0x1000;
constexpr u32 kData = 0x1800;

constexpr u32 branch(u32 f3, u32 rs1, u32 rs2, std::int32_t off) {
    return b_type(f3, rs1, rs2, off);
}

// A tiny assembler: words, labels, and branches, jumps and `la` to labels, resolved by words().
class Asm {
  public:
    explicit Asm(u32 base = kText) : base_(base) {}

    u32 here() const { return base_ + 4 * static_cast<u32>(words_.size()); }
    void put(u32 w) { words_.push_back(w); }
    void label(const std::string& name) { labels_[name] = here(); }
    u32 addr(const std::string& name) const { return labels_.at(name); }

    // rd := v (lui + addi).
    void li32(u32 rd, u32 v) {
        const u32 hi = (v + 0x800) >> 12;
        put(lui(rd, hi & 0xfffff));
        put(addi(rd, rd, static_cast<std::int32_t>(v - (hi << 12))));
    }
    void bc(u32 f3, u32 rs1, u32 rs2, const std::string& to) {
        fix(to, [=](u32 pc, u32 t) { return branch(f3, rs1, rs2, off(pc, t)); });
    }
    void j(u32 rd, const std::string& to) {
        fix(to, [=](u32 pc, u32 t) { return jal(rd, off(pc, t)); });
    }
    void la(u32 rd, const std::string& to) {
        fix(to, [=](u32, u32 t) { return lui(rd, ((t + 0x800) >> 12) & 0xfffff); });
        fix(to, [=](u32, u32 t) {
            return addi(rd, rd, static_cast<std::int32_t>(t - (((t + 0x800) >> 12) << 12)));
        });
    }
    // An ecall with a7 = n.
    void sys(u32 n) {
        put(li(a7, static_cast<std::int32_t>(n)));
        put(kEcall);
    }

    std::vector<u32> words() const {
        std::vector<u32> w = words_;
        for (const Fix& f : fixes_) {
            w[f.at] = f.make(base_ + 4 * static_cast<u32>(f.at), labels_.at(f.label));
        }
        return w;
    }
    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> b;
        for (const u32 w : words()) {
            for (int i = 0; i < 4; ++i) {
                b.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
            }
        }
        return b;
    }

  private:
    struct Fix {
        std::size_t at;
        std::string label;
        std::function<u32(u32, u32)> make; // (pc, target) -> word
    };
    static std::int32_t off(u32 pc, u32 t) { return static_cast<std::int32_t>(t - pc); }
    void fix(const std::string& label, std::function<u32(u32, u32)> make) {
        fixes_.push_back({words_.size(), label, std::move(make)});
        words_.push_back(0);
    }

    u32 base_;
    std::vector<u32> words_;
    std::map<std::string, u32> labels_;
    std::vector<Fix> fixes_;
};

std::vector<std::uint8_t> le_words(const std::vector<u32>& ws) {
    std::vector<std::uint8_t> b;
    for (const u32 w : ws) {
        for (int i = 0; i < 4; ++i) {
            b.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return b;
}

// The text at kText (executable), the data at kData (read-write, with `bss` bytes of zeros after).
std::vector<std::uint8_t> program(const Asm& text, const std::vector<u32>& data, u32 bss = 0) {
    ElfSpec spec;
    spec.entry = kText;
    spec.segments.push_back({kText, text.bytes(), 0});
    if (!data.empty()) {
        spec.segments.push_back(
            {kData, le_words(data), static_cast<u32>(4 * data.size()) + bss, 6});
    }
    return build_elf(spec);
}

malm::rv::Image image_of(const std::vector<std::uint8_t>& elf, u32 ram) {
    auto image = malm::rv::load_elf(elf, ram);
    INFO(image.error());
    REQUIRE(image.has_value());
    return *image;
}

// How a program ended, on either machine.
struct Outcome {
    std::array<u32, 32> x{};
    u32 pc = 0;
    u32 stop = 0;
    int status = -1;
    std::string out;
    std::vector<u32> data; // the RAM words from kData
};

StopCode stop_of(const std::string& fault) {
    for (const auto& [prefix, code] : {std::pair{"illegal instruction", StopCode::Illegal},
                                       {"ebreak", StopCode::Ebreak},
                                       {"misaligned jump target", StopCode::MisalignedJump},
                                       {"load from", StopCode::LoadFault},
                                       {"store to", StopCode::StoreFault},
                                       {"instruction fetch outside", StopCode::FetchFault}}) {
        if (fault.starts_with(prefix)) {
            return code;
        }
    }
    FAIL("unexpected fault " << fault);
    return StopCode::Running;
}

// The reference emulator, to the end, or until pc reaches `until` (code aot did not translate:
// there aot stops with Untranslated, after the same instructions).
Outcome reference(const std::vector<std::uint8_t>& elf, u32 ram, const std::string& input,
                  std::size_t data_words, std::optional<u32> until = std::nullopt) {
    malm::rv::Machine m(image_of(elf, ram), ram);
    StringIo io;
    io.in = input;
    Outcome o;
    for (std::uint64_t i = 0; i < 10'000'000; ++i) {
        if (until && m.pc() == *until) {
            o.stop = static_cast<u32>(StopCode::Untranslated);
            o.status = 1;
            break;
        }
        if (const auto stop = m.step(io)) {
            if (stop->kind == malm::rv::Stop::Kind::Exit) {
                o.stop = static_cast<u32>(StopCode::Exit);
                o.status = stop->exit_code;
            } else {
                o.stop = static_cast<u32>(stop_of(stop->message));
                o.status = 1;
            }
            break;
        }
    }
    o.x = m.regs();
    o.pc = m.pc();
    o.out = io.out;
    for (std::size_t i = 0; i < data_words; ++i) {
        o.data.push_back(*m.peek32(kData + 4 * static_cast<u32>(i)));
    }
    return o;
}

// A program translated by rv-aot and compiled once.
class Aot {
  public:
    Aot(const std::vector<std::uint8_t>& elf, u32 ram) {
        const auto src = malm::aot::aot_source(image_of(elf, ram), ram);
        INFO(src.error());
        REQUIRE(src.has_value());
        compiled_ = hlatest::compile(*src);
    }

    Outcome run(const std::string& input, std::size_t data_words, u32 ram) const {
        const hlatest::Ran r = hlatest::run(compiled_.assembly, input, 2'000'000'000);
        // A register the program never uses has no cell: it keeps its initial value.
        const auto reg = [&](const std::string& name, u32 initial) {
            const auto& labels = compiled_.assembly.layout.labels;
            return labels.contains("V_" + name) ? hlatest::reg(compiled_, r, name) : initial;
        };
        Outcome o;
        for (unsigned i = 1; i < 32; ++i) {
            o.x[i] = reg("x" + std::to_string(i), i == 2 ? ram : 0);
        }
        o.pc = reg("pc", 0);
        o.stop = reg("stop", 0);
        o.status = r.status;
        o.out = r.out;
        for (std::size_t i = 0; i < data_words; ++i) {
            o.data.push_back(hlatest::ram_word(r, kData + 4 * static_cast<u32>(i)));
        }
        return o;
    }

    const hlatest::Compiled& compiled() const { return compiled_; }

  private:
    hlatest::Compiled compiled_;
};

void check_same(const Outcome& want, const Outcome& got) {
    CHECK(got.stop == want.stop);
    CHECK(got.status == want.status);
    CHECK(got.pc == want.pc);
    CHECK(got.out == want.out);
    for (unsigned i = 1; i < 32; ++i) {
        INFO("x" << i);
        CHECK(hex(got.x[i]) == hex(want.x[i]));
    }
    CHECK(got.data == want.data);
}

} // namespace

// The control-flow graph.

TEST_CASE("rv-aot: leaders, indirect targets and resolved jumps") {
    Asm a;
    a.la(t0, "f");      // 0x1000: a code address built by lui + addi: an indirect target
    a.j(ra, "g");       // 0x1008: a call; 0x100C is a return address
    a.put(addi(t1, t1, 1));
    a.put(auipc(t2, 0)); // 0x1010: auipc + jalr resolve to 0x1020
    a.put(jalr(ra, t2, 0x10));
    a.bc(0, t1, t2, "end"); // 0x1018: a branch: its target and the next instruction
    a.put(addi(t1, t1, 2));
    a.label("f"); // 0x1020
    a.put(addi(t1, t1, 3));
    a.label("g"); // 0x1024
    a.put(jalr(0, ra, 0)); // ret: dynamic
    a.label("table_target"); // 0x1028, only named by a data word
    a.put(addi(t1, t1, 4));
    a.label("end"); // 0x102C
    a.sys(93);
    const auto elf = program(a, {a.addr("table_target"), 0x1001, kData});
    const auto cfg = malm::aot::recover_cfg(image_of(elf, 8192));
    REQUIRE(cfg.has_value());
    CHECK(cfg->code.size() == 13);
    CHECK(cfg->leaders == std::set<u32>{0x1000, 0x100C, 0x1010, 0x1018, 0x101C, 0x1020, 0x1024,
                                        0x1028, 0x102C});
    // The entry, the return addresses, f (la), the auipc's own address (a code address in a
    // register, so it might be a target), table_target (a data word); not 0x1001 or kData.
    CHECK(cfg->indirect == std::set<u32>{0x1000, 0x100C, 0x1010, 0x1018, 0x1020, 0x1028});
    CHECK(cfg->resolved == std::map<u32, u32>{{0x1014, 0x1020}});
}

TEST_CASE("rv-aot: a constant does not reach across a leader") {
    Asm a;
    a.put(auipc(t0, 0)); // 0x1000
    a.label("join");     // 0x1004: a branch target, so t0 is unknown here
    a.put(jalr(0, t0, 8));
    a.put(addi(t1, t1, 1));
    a.put(addi(t1, t1, 1));
    a.bc(1, t1, 0, "join");
    a.sys(93);
    const auto cfg = malm::aot::recover_cfg(image_of(program(a, {}), 8192));
    REQUIRE(cfg.has_value());
    CHECK(cfg->resolved.empty());
}

TEST_CASE("rv-aot: self-modifying code is a build error") {
    {
        Asm a;
        a.put(0x0000100F); // fence.i
        a.sys(93);
        const auto cfg = malm::aot::recover_cfg(image_of(program(a, {}), 8192));
        REQUIRE(!cfg.has_value());
        CHECK(cfg.error() ==
              "fence.i at 0x1000: self-modifying code is not supported in aot mode");
    }
    {
        Asm a;
        a.put(lui(t0, 1)); // t0 = 0x1000, the code
        a.put(addi(t1, 0, 0x13));
        a.put(store(0, t0, t1, 9)); // sb to 0x1009
        a.sys(93);
        const auto src = malm::aot::aot_source(image_of(program(a, {}), 8192), 8192);
        REQUIRE(!src.has_value());
        CHECK(src.error() ==
              "store to 0x1009 at 0x1008: self-modifying code is not supported in aot mode");
    }
    {
        Asm a; // the same store into the data segment is fine
        a.put(lui(t0, 1));
        a.put(store(0, t0, t1, 0x800));
        a.sys(93);
        CHECK(malm::aot::recover_cfg(image_of(program(a, {0}), 8192)).has_value());
    }
}

TEST_CASE("rv-aot: guest RAM and the dispatch table must fit HLA's RAM") {
    Asm a;
    a.sys(93);
    const auto elf = program(a, {});
    CHECK(malm::aot::aot_source(image_of(elf, 4u << 20), 4u << 20).error() ==
          "guest RAM and the dispatch table need 1048578 words of HLA RAM, more than its "
          "1048576: use a smaller --ram");
    CHECK(malm::aot::aot_source(image_of(elf, 1u << 20), 1u << 20).has_value());
}

// Translated programs.

namespace {

// Straight-line ALU code on random registers: every OP and OP-IMM form, lui and auipc, with x0
// as an operand and as rd, the same register twice and the immediates aot treats specially.
std::vector<std::uint8_t> alu_program(int n) {
    std::mt19937 gen(29);
    const auto rng = [&] { return static_cast<u32>(gen()); };
    const auto any = [&]() -> u32 {
        static constexpr std::array<u32, 8> kEdges = {0,          1,          0x7FFFFFFF, 0x80000000,
                                                      0xFFFFFFFF, 0x12345678, 0xFF,       31};
        return rng() % 2 ? kEdges[rng() % kEdges.size()] : rng();
    };
    const auto reg = [&]() -> u32 { return rng() % 4 == 0 ? 0 : rng() % 32; };
    const auto imm12 = [&]() -> std::int32_t {
        static constexpr std::array<std::int32_t, 5> kEdges = {0, -1, 2047, -2048, 1};
        return rng() % 2 ? kEdges[rng() % kEdges.size()]
                         : static_cast<std::int32_t>(rng() % 4096) - 2048;
    };
    Asm a;
    for (u32 r = 1; r < 32; ++r) {
        a.li32(r, any());
    }
    for (int i = 0; i < n; ++i) {
        const u32 rd = reg(), rs1 = reg(), rs2 = rng() % 5 == 0 ? rs1 : reg();
        const u32 f3 = rng() % 8;
        switch (rng() % 8) {
        case 0:
        case 1: { // OP, RV32M a third of the time
            const u32 kind = rng() % 3;
            u32 f7 = kind == 0 ? 1 : 0;
            if (kind == 2 && (f3 == 0 || f3 == 5)) {
                f7 = 0x20; // sub, sra
            }
            a.put(op(f3, f7, rd, rs1, rs2));
            break;
        }
        case 2:
        case 3:
        case 4: { // OP-IMM
            std::int32_t imm = imm12();
            if (f3 == 1 || f3 == 5) {
                imm = static_cast<std::int32_t>(rng() % 32) | (f3 == 5 && rng() % 2 ? 0x400 : 0);
            }
            a.put(i_type(0x13, rd, f3, rs1, imm));
            break;
        }
        case 5:
            a.put(lui(rd, any() & 0xfffff));
            break;
        case 6:
            a.put(auipc(rd, any() & 0xfffff));
            break;
        default: // add, addi with x0 (mv, li)
            a.put(rng() % 2 ? op(0, 0, rd, 0, rs2) : addi(rd, rs1, rng() % 2 ? 0 : imm12()));
            break;
        }
    }
    a.sys(93);
    return program(a, {});
}

} // namespace

TEST_CASE("rv-aot: ALU instructions match the reference emulator" *
          kSlowInDebug) {
    const auto elf = alu_program(kDebugBuild ? 60 : 200);
    const Aot aot(elf, 8192);
    check_same(reference(elf, 8192, "", 0), aot.run("", 0, 8192));
}

namespace {

// Loads and stores of every width at every alignment, branches, calls (direct, through a
// resolved jalr and through the dispatch table), a jump table, a loop and the system calls.
// The first input byte picks a case of the jump table; s0 accumulates what happened.
struct ControlProgram {
    std::vector<std::uint8_t> elf;
    static constexpr std::size_t kDataWords = 16;
};

ControlProgram control_program() {
    Asm a;
    const auto mix = [&](u32 r) { // s0 := s0 * 3 + r
        a.put(op(0, 0, s0, s0, r));
        a.put(i_type(0x13, t2, 1, s0, 1));
        a.put(op(0, 0, s0, s0, t2));
    };
    a.li32(s1, kData);
    // read(0, data + 48, 4)
    a.put(li(a0, 0));
    a.put(addi(a1, s1, 48));
    a.put(li(a2, 4));
    a.sys(63);
    a.put(addi(s2, a0, 0));
    mix(s2);
    // Loads: data[0..8) = BB AA 99 88 FF EE DD CC.
    for (std::int32_t off = 0; off < 4; ++off) {
        for (const u32 f3 : {0u, 1u, 2u, 4u, 5u}) {
            a.put(load(f3, t0, s1, off));
            mix(t0);
        }
    }
    a.put(load(2, 0, s1, 0)); // a load into x0
    // Stores into data[28..40), then read back.
    a.li32(t1, 0xA1B2C3D4);
    for (std::int32_t off = 0; off < 4; ++off) {
        a.put(store(0, s1, t1, 28 + off));
        a.put(store(1, s1, t1, 32 + off));
        a.put(store(2, s1, t1, 36 + off));
        a.put(i_type(0x13, t1, 5, t1, 3));
    }
    a.put(store(2, s1, 0, 44)); // sw zero
    for (const std::int32_t off : {28, 32, 36, 40}) {
        a.put(load(2, t0, s1, off));
        mix(t0);
    }
    // Branches, taken and not: (t0, t1) = (-1, 1) and (5, 5); and the x0 forms.
    int n = 0;
    for (const auto& [x, y] : {std::pair{-1, 1}, std::pair{5, 5}}) {
        a.put(li(t0, x));
        a.put(li(t1, y));
        for (const u32 f3 : {0u, 1u, 4u, 5u, 6u, 7u}) {
            const std::string skip = "skip" + std::to_string(n++);
            a.bc(f3, t0, t1, skip);
            a.put(addi(s0, s0, n));
            a.label(skip);
        }
        for (const auto& [f3, r1, r2] : {std::tuple{0u, t0, 0u}, std::tuple{1u, 0u, t0},
                                         std::tuple{4u, t0, 0u}, std::tuple{5u, 0u, t0},
                                         std::tuple{0u, 0u, 0u}, std::tuple{1u, 0u, 0u}}) {
            const std::string skip = "skip" + std::to_string(n++);
            a.bc(f3, r1, r2, skip);
            a.put(addi(s0, s0, n));
            a.label(skip);
        }
    }
    // Calls: direct, through a resolved jalr, through the table (also at an odd offset).
    a.j(ra, "f1");
    a.la(t2, "f2");
    a.put(jalr(ra, t2, 0));
    a.put(load(2, t2, s1, 8)); // the address of f3
    a.put(jalr(ra, t2, 0));
    a.put(jalr(ra, t2, 1));
    a.put(addi(t2, t2, -4)); // jalr with an offset: t2 + 4 = f3
    a.put(jalr(ra, t2, 4));
    mix(ra);
    // The jump table: case (first input byte & 3).
    a.put(load(4, t0, s1, 48));
    a.put(i_type(0x13, t0, 7, t0, 3));
    a.put(i_type(0x13, t0, 1, t0, 2));
    a.put(op(0, 0, t0, t0, s1));
    a.put(load(2, t0, t0, 12));
    a.put(jalr(0, t0, 0));
    for (int c = 0; c < 4; ++c) {
        a.label("case" + std::to_string(c));
        a.put(addi(s0, s0, 11 * (c + 1)));
        a.j(0, "join");
    }
    a.label("join");
    // A loop: t0 from 5 down.
    a.put(li(t0, 5));
    a.label("loop");
    mix(t0);
    a.put(addi(t0, t0, -1));
    a.bc(1, t0, 0, "loop");
    // write(1, data + 48, read), write(1, data, 4); bad fd, bad buffer, unknown call; brk.
    a.put(li(a0, 1));
    a.put(addi(a1, s1, 48));
    a.put(addi(a2, s2, 0));
    a.sys(64);
    mix(a0);
    a.put(li(a0, 2));
    a.put(addi(a1, s1, 0));
    a.put(li(a2, 4));
    a.sys(64);
    a.put(li(a0, 5));
    a.sys(64);
    mix(a0);
    a.put(li(a0, 1));
    a.put(li(a1, -256));
    a.sys(64);
    mix(a0);
    a.sys(999);
    mix(a0);
    a.put(li(a0, 0));
    a.sys(214);
    mix(a0);
    a.put(addi(a0, a0, 64));
    a.sys(214);
    mix(a0);
    a.put(li(a0, 1));
    a.sys(214);
    mix(a0);
    a.put(op(0, 1, s3, s0, s2)); // mul
    a.put(op(5, 1, s4, s0, s2)); // divu, by zero without input
    a.put(addi(a0, s0, 0));
    a.sys(93);
    a.label("f1");
    a.put(addi(s0, s0, 100));
    a.put(jalr(0, ra, 0));
    a.label("f2");
    a.put(addi(s0, s0, 200));
    a.put(jalr(0, ra, 0));
    a.label("f3");
    a.put(addi(s0, s0, 300));
    a.put(jalr(0, ra, 0));

    std::vector<u32> data(ControlProgram::kDataWords);
    data[0] = 0x8899AABB;
    data[1] = 0xCCDDEEFF;
    data[2] = a.addr("f3");
    for (int c = 0; c < 4; ++c) {
        data[3 + static_cast<std::size_t>(c)] = a.addr("case" + std::to_string(c));
    }
    return {program(a, data)};
}

} // namespace

TEST_CASE("rv-aot: memory, branches, calls and system calls match the reference emulator" *
          kSlowInDebug) {
    const ControlProgram p = control_program();
    // Guest RAM not a power of two: the range checks are sltu.
    constexpr u32 kRam = 8192 + 16;
    const Aot aot(p.elf, kRam);
    for (const std::string input : {"", "a", "\x01\x02\x03\x04", "xyz"}) {
        INFO("input \"" << input << "\"");
        check_same(reference(p.elf, kRam, input, ControlProgram::kDataWords),
                   aot.run(input, ControlProgram::kDataWords, kRam));
    }
}

namespace {

// Faults and jumps out of the translated code: the input byte picks a case through a jump table.
// untranslated: where the case continues at code aot did not translate (the reference emulator
// executes it).
struct FaultCase {
    std::string name;
    std::optional<u32> untranslated;
};

constexpr u32 kFaultRam = 8192;

std::pair<std::vector<std::uint8_t>, std::vector<FaultCase>> fault_program() {
    Asm a;
    std::vector<FaultCase> cases;
    std::vector<std::string> labels;
    const auto add = [&](std::string name, std::optional<u32> untranslated = std::nullopt) {
        labels.push_back(name);
        a.label(name);
        cases.push_back({std::move(name), untranslated});
    };
    a.li32(s1, kData);
    a.put(li(a0, 0));
    a.put(addi(a1, s1, 0x58));
    a.put(li(a2, 1));
    a.sys(63);
    a.put(load(4, t0, s1, 0x58));
    a.put(i_type(0x13, t0, 1, t0, 2));
    a.put(op(0, 0, t0, t0, s1));
    a.put(load(2, t0, t0, 0));
    a.put(li(t1, 1));
    a.put(jalr(ra, t0, 0));

    add("illegal");
    a.put(0xFFFFFFFF);
    add("illegal load funct3");
    a.put(load(3, t0, s1, 0));
    add("csr");
    a.put(0x30001073);
    add("ebreak");
    a.put(kEbreak);
    add("beq x0, x0 to a misaligned target");
    a.put(branch(0, 0, 0, 6));
    add("blt to a misaligned target");
    a.put(branch(4, 0, t1, 6));
    add("jal to a misaligned target");
    a.put(jal(ra, 2));
    add("jalr to a misaligned target");
    a.put(load(2, t2, s1, 0x50)); // the address of "f"
    a.put(jalr(ra, t2, 2));
    add("jalr to a misaligned target, rd = rs1");
    a.put(load(2, t2, s1, 0x50));
    a.put(jalr(t2, t2, 2));
    add("jalr outside guest RAM");
    a.put(load(2, t2, s1, 0x54)); // 0x80000000
    a.put(jalr(ra, t2, 0));
    add("resolved jalr outside guest RAM");
    a.put(lui(t2, 0x80000));
    a.put(jalr(ra, t2, 0));
    add("load outside guest RAM");
    a.put(li(t2, -16));
    a.put(load(2, t0, t2, 0));
    add("store outside guest RAM");
    a.li32(t2, kFaultRam);
    a.put(store(2, t2, t2, 0));
    add("lw across the end of guest RAM");
    a.li32(t2, kFaultRam - 2);
    a.put(load(2, t0, t2, 0));
    add("sh across the end of guest RAM");
    a.li32(t2, kFaultRam - 1);
    a.put(store(1, t2, t2, 0));
    add("exit");
    a.put(li(a0, 42));
    a.sys(93);
    add("jalr into data", kData);
    a.put(addi(t2, s1, 0));
    a.put(jalr(ra, t2, 0));
    add("jalr into the middle of a block");
    a.put(load(2, t2, s1, 0x50));
    a.put(addi(t2, t2, 4));
    a.put(jalr(ra, t2, 0));
    add("jal into data", kData);
    a.put(jal(ra, static_cast<std::int32_t>(kData - a.here())));
    a.label("f");
    a.put(addi(s0, s0, 1));
    a.put(addi(s0, s0, 2)); // f + 4: no indirect target
    a.put(jalr(0, ra, 0));
    add("off the end of the code");
    a.put(addi(s0, s0, 3));
    const u32 end = a.here();
    for (FaultCase& c : cases) {
        if (c.name == "jalr into the middle of a block") {
            c.untranslated = a.addr("f") + 4;
        }
    }
    cases.back().untranslated = end;

    // The cases' addresses, then (at 0x50) f and a word outside guest RAM; input at 0x58.
    std::vector<u32> data(24);
    REQUIRE(labels.size() <= 20);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        data[i] = a.addr(labels[i]);
    }
    data[20] = a.addr("f");
    data[21] = 0x80000000;
    return {program(a, data), cases};
}

} // namespace

TEST_CASE("rv-aot: faults and jumps out of the code stop like the reference emulator" *
          kSlowInDebug) {
    const auto [elf, cases] = fault_program();
    const Aot aot(elf, kFaultRam);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        INFO(cases[i].name);
        const std::string input(1, static_cast<char>(i));
        const Outcome want = reference(elf, kFaultRam, input, 0, cases[i].untranslated);
        if (cases[i].untranslated) {
            CHECK(want.pc == *cases[i].untranslated);
        } else {
            CHECK(want.stop != static_cast<u32>(StopCode::Untranslated));
        }
        check_same(want, aot.run(input, 0, kFaultRam));
    }
}

// VM steps per guest instruction class (docs/rv.md 9 records them): each class is a run of the
// same instruction, from one block to the next, measured by the arrivals at the blocks' labels.
TEST_CASE("rv-aot: the cost of instructions" * kSlowInDebug) {
    struct Class {
        std::string name;
        int repeat;
        std::function<void(Asm&)> emit; // one instance
    };
    const std::vector<Class> classes = {
        {"lui", 8, [](Asm& a) { a.put(lui(t0, 0x12345)); }},
        {"addi", 8, [](Asm& a) { a.put(addi(t0, t1, 77)); }},
        {"addi rd, rs, 0 (mv)", 8, [](Asm& a) { a.put(addi(t0, t1, 0)); }},
        {"ori", 8, [](Asm& a) { a.put(i_type(0x13, t0, 6, t1, 0x5A)); }},
        {"srli 7", 8, [](Asm& a) { a.put(i_type(0x13, t0, 5, t1, 7)); }},
        {"slli 7", 8, [](Asm& a) { a.put(i_type(0x13, t0, 1, t1, 7)); }},
        {"slti", 8, [](Asm& a) { a.put(i_type(0x13, t0, 2, t1, -5)); }},
        {"add", 8, [](Asm& a) { a.put(op(0, 0, t0, t1, t2)); }},
        {"xor", 8, [](Asm& a) { a.put(op(4, 0, t0, t1, t2)); }},
        {"sll", 8, [](Asm& a) { a.put(op(1, 0, t0, t1, t2)); }},
        {"sltu", 8, [](Asm& a) { a.put(op(3, 0, t0, t1, t2)); }},
        {"mul", 2, [](Asm& a) { a.put(op(0, 1, t0, t1, t2)); }},
        {"divu", 2, [](Asm& a) { a.put(op(5, 1, t0, t1, t2)); }},
        {"lw", 8, [](Asm& a) { a.put(load(2, t0, s1, 4)); }},
        {"lb", 8, [](Asm& a) { a.put(load(0, t0, s1, 5)); }},
        {"lbu", 8, [](Asm& a) { a.put(load(4, t0, s1, 5)); }},
        {"sw", 8, [](Asm& a) { a.put(store(2, s1, t1, 8)); }},
        {"sb", 8, [](Asm& a) { a.put(store(0, s1, t1, 9)); }},
        {"beq not taken", 8, [](Asm& a) { a.put(branch(0, t1, t2, 8)); }},
        {"beqz not taken", 8, [](Asm& a) { a.put(branch(0, t1, 0, 8)); }},
        {"bltu not taken", 8, [](Asm& a) { a.put(branch(6, t1, t2, 8)); }},
        {"bne taken", 8, [](Asm& a) { a.put(branch(1, t1, t2, 4)); }},
        {"jal", 8, [](Asm& a) { a.put(jal(0, 4)); }},
        {"jalr (call) + ret (both dispatched)", 4,
         [](Asm& a) { a.put(jalr(ra, a3, 0)); }},
        {"ecall brk, with li a0 and li a7", 2,
         [](Asm& a) {
             a.put(li(a0, 0));
             a.sys(214);
         }},
    };
    Asm a;
    a.li32(s1, kData);
    a.li32(t1, 0x12345678);
    a.li32(t2, 0x0000F00D);
    a.put(load(2, a3, s1, 0)); // the address of ret_f
    std::vector<std::string> starts;
    for (std::size_t i = 0; i <= classes.size(); ++i) {
        const std::string start = "c" + std::to_string(i);
        a.j(0, start); // a block of its own
        a.label(start);
        starts.push_back(start);
        if (i < classes.size()) {
            for (int k = 0; k < classes[i].repeat; ++k) {
                classes[i].emit(a);
            }
        }
    }
    a.sys(93);
    a.label("ret_f");
    a.put(jalr(0, ra, 0));
    const auto elf = program(a, {a.addr("ret_f"), 0x11223344, 0x55667788, 0});
    const Aot aot(elf, 8192);
    check_same(reference(elf, 8192, "", 4), aot.run("", 4, 8192));

    // The arrivals: the unit _MOVED jumps through the label's pad (docs/hla.md 4.1).
    const auto& labels = aot.compiled().assembly.layout.labels;
    const malm::core::Value moved_jmp = *labels.at("_MOVED").addr.incremented();
    std::vector<malm::core::Value> pads;
    for (const std::string& start : starts) {
        char name[16];
        std::snprintf(name, sizeof name, "L_b_%X", a.addr(start));
        const auto it = labels.find(name);
        REQUIRE(it != labels.end());
        pads.push_back(it->second.addr);
    }
    malm::core::Vm vm(aot.compiled().assembly.startup.text);
    hlatest::ScriptIo io;
    std::vector<std::uint64_t> at(starts.size());
    std::uint64_t first = 0;
    while (vm.step(io).status == malm::core::Status::Continue && vm.steps() < 100'000'000) {
        if (vm.c() == moved_jmp) {
            for (std::size_t i = 0; i < pads.size(); ++i) {
                if (vm.d() == pads[i] && at[i] == 0) {
                    at[i] = vm.steps();
                    first = first == 0 ? vm.steps() : first;
                }
            }
        }
    }
    std::string table;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        REQUIRE(at[i + 1] > at[i]);
        char line[96];
        std::snprintf(line, sizeof line, "%-38s %8.0f\n", classes[i].name.c_str(),
                      static_cast<double>(at[i + 1] - at[i]) / classes[i].repeat);
        table += line;
    }
    MESSAGE("startup: " << first << " steps; steps per instruction (each run includes one jal):\n"
                        << table);
}
