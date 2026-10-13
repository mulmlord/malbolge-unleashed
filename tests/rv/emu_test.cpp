// rv-emu (issue #25) against the C++ reference emulator, one instruction at a time.
//
// The emulator is compiled once per RAM size, as a harness (a stop continues at `fetch` instead of
// halting), and runs in one VM. A case is one instruction: the test pokes the guest
// registers, pc, the program break and all of RAM into the VM, runs it to its next arrival at
// `fetch` and compares the registers, pc, the break, RAM,
// the output, the input consumed and the stop code with rv::Machine after one step from the same
// state. The steps between two arrivals are the instruction's cost.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../hla/hla_test_util.hpp"
#include "rv/emu.hpp"
#include "rv/machine.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;
using malm::core::Value;
using malm::rv::sem::StopCode;

namespace {

// Guest RAM: a power of two (the range check is an `and`), and a size that is none and not a
// multiple of 256 either (the check is a `sltu`, and pc += 4 at its last word takes the add).
constexpr u32 kRamPow2 = 4096;
constexpr u32 kRamOdd = 4096 + 16;

// A debug build has sanitizers, and its assembler and VM are ~70x slower: compiling the emulator
// takes it about 3 minutes, so there the test is slow (nightly), and runs fewer cases.
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

struct Case {
    std::string what; // the class, for the cost table
    u32 ram = 0;      // the guest RAM size
    u32 insn = 0;
    u32 pc = 0x100;
    std::array<u32, 32> x{};
    std::vector<std::pair<u32, u32>> words; // RAM words (address, value) besides the instruction
    std::string input;
    u32 brk = 0x800;
};

// The state both machines are compared on.
struct State {
    std::array<u32, 32> x{};
    u32 pc = 0;
    u32 brk = 0;
    u32 stop = 0;
    std::vector<u32> ram;
    std::string out;
    std::size_t in = 0; // input bytes consumed
};

// RAM before a case: a fixed pattern of random words, then the case's words and instruction.
std::vector<u32> initial_ram(const Case& c) {
    std::vector<u32> ram(c.ram / 4);
    std::mt19937 rng(0xE3);
    for (u32& w : ram) {
        w = static_cast<u32>(rng());
    }
    for (const auto& [addr, v] : c.words) {
        ram[addr / 4] = v;
    }
    ram[c.pc / 4] = c.insn;
    return ram;
}

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

// One step of the reference emulator. Where it continues at a pc outside RAM, rv-emu stops at
// once (its fetch check runs with the jump), so that is a FetchFault here too.
State reference(const Case& c) {
    const std::vector<u32> ram = initial_ram(c);
    std::vector<std::uint8_t> bytes(c.ram);
    for (u32 a = 0; a < c.ram; ++a) {
        bytes[a] = static_cast<std::uint8_t>(ram[a / 4] >> (8 * (a % 4)));
    }
    malm::rv::Machine m({c.pc, {{0, c.ram, std::move(bytes), true}}, c.brk}, c.ram);
    for (unsigned i = 1; i < 32; ++i) {
        m.set_reg(i, c.x[i]);
    }
    StringIo io;
    io.in = c.input;
    const std::optional<malm::rv::Stop> stop = m.step(io);
    State s;
    s.x = m.regs();
    s.pc = m.pc();
    s.brk = m.brk();
    if (!stop) {
        s.stop = static_cast<u32>(m.pc() < c.ram ? StopCode::Running : StopCode::FetchFault);
    } else if (stop->kind == malm::rv::Stop::Kind::Exit) {
        s.stop = static_cast<u32>(StopCode::Exit);
    } else {
        s.stop = static_cast<u32>(stop_of(stop->message));
    }
    for (u32 a = 0; a < c.ram; a += 4) {
        s.ram.push_back(*m.peek32(a));
    }
    s.out = io.out;
    s.in = io.pos;
    return s;
}

// The emulator in its VM, stopped at an arrival at `fetch`.
class Emu {
  public:
    explicit Emu(u32 ram) : ram_(ram) {
        malm::rv::EmuOptions opt;
        opt.ram_size = ram;
        opt.harness = true;
        compiled_ = hlatest::compile(malm::rv::emu_source(opt));
        vm_.emplace(compiled_.assembly.startup.text);
        fetch_ = label("L_fetch");
        moved_jmp_ = *label("_MOVED").incremented();
        for (unsigned i = 1; i < 32; ++i) {
            x_[i] = label("V_x" + std::to_string(i));
        }
        pc_ = label("V_pc");
        brk_ = label("V_brk");
        brkmin_ = label("V_brkmin");
        stop_ = label("V_stop");
        hlatest::ScriptIo io;
        REQUIRE(to_fetch(io, 100'000'000)); // the startup code
        startup_steps = vm_->steps();
    }

    // Runs one case; nullopt if the VM halted, faulted or ran out of steps.
    std::optional<State> run(const Case& c, std::uint64_t& steps) {
        for (unsigned i = 1; i < 32; ++i) {
            put(x_[i], c.x[i]);
        }
        put(pc_, c.pc);
        put(brk_, c.brk);
        put(brkmin_, c.brk);
        put(stop_, 0);
        const std::vector<u32> ram = initial_ram(c);
        REQUIRE(c.ram == ram_);
        for (u32 a = 0; a < ram_; a += 4) {
            vm_->poke(malm::hla::ram_record(a), malm::hla::word(ram[a / 4]));
        }
        hlatest::ScriptIo io;
        io.in = c.input;
        const std::uint64_t start = vm_->steps();
        if (!to_fetch(io, 3'000'000)) {
            return std::nullopt;
        }
        steps = vm_->steps() - start;
        State s;
        for (unsigned i = 1; i < 32; ++i) {
            s.x[i] = get(x_[i]);
        }
        s.pc = get(pc_);
        s.brk = get(brk_);
        s.stop = get(stop_);
        for (u32 a = 0; a < ram_; a += 4) {
            s.ram.push_back(get(malm::hla::ram_record(a)));
        }
        s.out = io.out;
        s.in = io.pos;
        return s;
    }

    std::uint64_t startup_steps = 0;

  private:
    Value label(const std::string& name) const {
        const auto it = compiled_.assembly.layout.labels.find(name);
        REQUIRE(it != compiled_.assembly.layout.labels.end());
        return it->second.addr;
    }
    u32 get(Value at) const {
        const auto w = malm::hla::word_value(vm_->memory().read(at));
        return w ? *w : 0xDEADBEEF;
    }
    void put(Value at, u32 v) { vm_->poke(at, malm::hla::word(v)); }

    // An arrival at `fetch`: the unit _MOVED jumps through the label's pad (docs/hla.md 4.1), its
    // Jmp at C reading the pad at D. The startup code passes D over the pad too, from elsewhere.
    bool at_fetch() const { return vm_->d() == fetch_ && vm_->c() == moved_jmp_; }

    // Steps to the next arrival at `fetch` (at least one step).
    bool to_fetch(malm::core::Io& io, std::uint64_t budget) {
        const std::uint64_t end = vm_->steps() + budget;
        do {
            if (vm_->step(io).status != malm::core::Status::Continue) {
                return false;
            }
        } while (!at_fetch() && vm_->steps() < end);
        return at_fetch();
    }

    u32 ram_;
    hlatest::Compiled compiled_;
    std::optional<malm::core::Vm> vm_;
    Value fetch_, moved_jmp_, pc_, brk_, brkmin_, stop_;
    std::array<Value, 32> x_{};
};

// Case generation.

class Gen {
  public:
    explicit Gen(u32 ram_size) : ram(ram_size) {}

    const u32 ram;
    std::vector<Case> cases;

    u32 any() {
        static constexpr std::array<u32, 12> kEdges = {
            0,          1,          2,          0x7F,       0x80,   0xFF,
            0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFF, 0x12345678};
        return rng_() % 2 ? kEdges[rng_() % kEdges.size()] : static_cast<u32>(rng_());
    }
    u32 reg() { return rng_() % 32; }
    u32 below(u32 n) { return static_cast<u32>(rng_() % n); }
    std::int32_t imm12() {
        static constexpr std::array<std::int32_t, 6> kEdges = {-2048, -1, 0, 1, 2047, 4};
        return rng_() % 3 == 0 ? kEdges[rng_() % kEdges.size()]
                               : static_cast<std::int32_t>(rng_() % 4096) - 2048;
    }
    // A case with random registers and an aligned pc inside RAM; x0 = 0.
    Case make(std::string what, u32 insn) {
        Case c;
        c.what = std::move(what);
        c.ram = ram;
        c.insn = insn;
        c.pc = below(ram / 4) * 4;
        for (unsigned i = 1; i < 32; ++i) {
            c.x[i] = any();
        }
        return c;
    }
    void add(Case c) {
        c.x[0] = 0;
        cases.push_back(std::move(c));
    }

    // An address for a `size`-byte access: mostly inside RAM (any alignment), some across its end
    // or far outside.
    u32 address(u32 size) {
        switch (rng_() % 8) {
        case 0:
            return ram - size + below(2 * size); // the last bytes, and across the end
        case 1: {
            const std::array<u32, 4> out = {ram, 0xFFFFFFFF, 0x80000000, 0xFFFFF000};
            return out[rng_() % out.size()];
        }
        default:
            return below(ram);
        }
    }

  private:
    std::mt19937 rng_{25};
};

constexpr std::array<const char*, 8> kOpNames = {"add", "sll", "slt", "sltu",
                                                 "xor", "srl", "or",  "and"};
constexpr std::array<const char*, 8> kMulNames = {"mul", "mulh", "mulhsu", "mulhu",
                                                  "div", "divu", "rem",    "remu"};

void alu_cases(Gen& g, int n, int n_muldiv) {
    for (u32 f3 = 0; f3 < 8; ++f3) {
        for (const u32 f7 : {0u, 0x20u, 0x01u}) {
            if (f7 == 0x20 && f3 != 0 && f3 != 5) {
                continue;
            }
            const std::string name = f7 == 1      ? kMulNames[f3]
                                     : f7 == 0x20 ? (f3 == 0 ? "sub" : "sra")
                                                  : kOpNames[f3];
            for (int i = 0; i < (f7 == 1 ? n_muldiv : n); ++i) {
                const u32 rd = g.reg(), rs1 = g.reg();
                const u32 rs2 = i % 4 == 0 ? rs1 : g.reg(); // the same register twice
                Case c = g.make(name, op(f3, f7, i % 5 == 0 ? rs1 : rd, rs1, rs2));
                if (f7 == 1 && i % 3 == 0) {
                    c.x[rs2] = i % 2 ? 0 : 0xFFFFFFFF; // division by zero, INT_MIN / -1
                    c.x[rs1] = i % 2 ? c.x[rs1] : 0x80000000;
                }
                g.add(c);
            }
        }
    }
    constexpr std::array<const char*, 8> kImmNames = {"addi", "slli", "slti", "sltiu",
                                                      "xori", "srxi", "ori",  "andi"};
    for (u32 f3 = 0; f3 < 8; ++f3) {
        for (int i = 0; i < n; ++i) {
            const u32 rd = g.reg(), rs1 = g.reg();
            std::int32_t imm = g.imm12();
            std::string name = kImmNames[f3];
            if (f3 == 1 || f3 == 5) { // shamt, srai has 0x400
                imm = static_cast<std::int32_t>(g.below(32));
                name = f3 == 1 ? "slli" : "srli";
                if (f3 == 5 && i % 2) {
                    imm |= 0x400;
                    name = "srai";
                }
            }
            g.add(g.make(name, i_type(0x13, rd, f3, rs1, imm)));
        }
    }
    for (int i = 0; i < n; ++i) {
        g.add(g.make("lui", lui(g.reg(), g.below(1u << 20))));
        g.add(g.make("auipc", auipc(g.reg(), g.below(1u << 20))));
    }
}

void memory_cases(Gen& g, int n) {
    constexpr std::array<const char*, 6> kLoads = {"lb", "lh", "lw", "", "lbu", "lhu"};
    constexpr std::array<const char*, 3> kStores = {"sb", "sh", "sw"};
    for (u32 f3 = 0; f3 < 6; ++f3) {
        if (f3 == 3) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const u32 rd = g.reg(), rs1 = g.reg() | 1; // a nonzero base
            const std::int32_t imm = g.imm12();
            const u32 size = 1u << (f3 & 3);
            const u32 addr = g.address(size);
            Case c = g.make(std::string(kLoads[f3]) + (addr % size ? " (misaligned)" : ""),
                            load(f3, rd, rs1, imm));
            c.x[rs1] = addr - static_cast<u32>(imm);
            g.add(c);
        }
    }
    for (u32 f3 = 0; f3 < 3; ++f3) {
        for (int i = 0; i < n; ++i) {
            const u32 rs1 = g.reg() | 1, rs2 = g.reg();
            const std::int32_t imm = g.imm12();
            const u32 size = 1u << f3;
            const u32 addr = g.address(size);
            Case c = g.make(std::string(kStores[f3]) + (addr % size ? " (misaligned)" : ""),
                            store(f3, rs1, rs2, imm));
            c.x[rs1] = addr - static_cast<u32>(imm);
            g.add(c);
        }
    }
}

void control_cases(Gen& g, int n) {
    constexpr std::array<const char*, 8> kBranches = {"beq", "bne", "",     "",
                                                      "blt", "bge", "bltu", "bgeu"};
    for (u32 f3 = 0; f3 < 8; ++f3) {
        if (f3 == 2 || f3 == 3) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const u32 rs1 = g.reg(), rs2 = g.reg();
            // Targets mostly in RAM, some misaligned (offsets are even) or outside.
            Case c = g.make(kBranches[f3], 0);
            const u32 target = i % 7 == 0    ? g.below(g.ram / 2) * 2
                               : i % 11 == 0 ? g.ram
                                             : g.below(g.ram / 4) * 4;
            const auto off = static_cast<std::int32_t>(target - c.pc);
            c.insn = b_type(f3, rs1, rs2, std::clamp(off, -4096, 4094) & ~1);
            if (i % 3 == 0) {
                c.x[rs2] = c.x[rs1]; // equal
            } else if (i % 3 == 1) {
                c.x[rs2] = c.x[rs1] ^ (g.below(2) ? 0x80000000 : 1); // the sign or the low bit
            }
            g.add(c);
        }
        for (int i = 0; i < n / 4; ++i) { // a loop's branch: backwards over a short distance
            Case c = g.make(kBranches[f3], b_type(f3, 5, 6, -8));
            c.pc = 0x200;
            g.add(c);
        }
    }
    for (int i = 0; i < n; ++i) {
        Case c = g.make("jal", 0);
        const u32 target = i % 5 == 0 ? g.below(g.ram / 2) * 2 : g.below(g.ram / 4) * 4;
        const std::int32_t off = i % 9 == 0
                                     ? static_cast<std::int32_t>(g.below(1u << 21)) - (1 << 20)
                                     : static_cast<std::int32_t>(target - c.pc);
        c.insn = jal(g.reg(), off & ~1);
        g.add(c);
    }
    for (int i = 0; i < n; ++i) {
        const u32 rd = g.reg(), rs1 = g.reg() | 1;
        const std::int32_t imm = g.imm12();
        Case c = g.make("jalr", jalr(i % 4 == 0 ? rs1 : rd, rs1, imm));
        const u32 target = i % 6 == 0 ? g.any() : g.below(g.ram); // bit 0 is cleared
        c.x[rs1] = target - static_cast<u32>(imm);
        g.add(c);
    }
    // pc += 4 at the end of a 256-byte block (the carry path) and of RAM (a fetch fault).
    for (const u32 pc : {0xFCu, 0x2FCu, g.ram - 0x104, g.ram - 4}) {
        Case c = g.make("addi (pc edge)", addi(5, 6, 7));
        c.pc = pc;
        g.add(c);
    }
}

void system_cases(Gen& g) {
    // fence, fence.i, then reserved funct3 values.
    for (u32 f3 = 0; f3 < 4; ++f3) {
        g.add(g.make("fence", i_type(0x0F, g.reg(), f3, g.reg(), 0x0FF)));
    }
    // System calls, each with a few argument sets.
    const auto sys = [&](u32 n, u32 a0, u32 a1, u32 a2, std::string input = {}) {
        Case c = g.make("ecall", kEcall);
        c.x[17] = n;
        c.x[10] = a0;
        c.x[11] = a1;
        c.x[12] = a2;
        c.input = std::move(input);
        g.add(c);
    };
    for (const u32 fd : {1u, 2u}) {
        sys(64, fd, 0x400, 5);
        sys(64, fd, g.ram - 3, 3);
    }
    sys(64, 1, 0x400, 0);
    sys(64, 0, 0x400, 4);     // EBADF
    sys(64, 0x101, 0x400, 4); // EBADF: the switch sees fd 1
    sys(64, 1, g.ram - 3, 4); // EFAULT
    sys(64, 1, 0xFFFFFFFF, 2);
    sys(64, 1, 4, 0xFFFFFFFF);
    sys(63, 0, 0x404, 6, "hello, world"); // a full read
    sys(63, 0, g.ram - 2, 2, "xyz");
    sys(63, 1, 0x400, 3, "abc");       // EBADF
    sys(63, 0, g.ram - 2, 3, "abc");   // EFAULT
    sys(63 + 256, 0, 0x400, 3, "abc"); // ENOSYS: the switch sees 63
    sys(93, 0x1234, 0, 0);
    sys(93, 0, 0, 0);
    for (const u32 addr : {0u, 0x800u, 0x900u, 0x7FFu, g.ram, g.ram + 1, 0xFFFFFFFFu}) {
        sys(214, addr, 0, 0);
    }
    sys(255, 1, 2, 3);       // the register dump
    sys(255 + 256, 0, 0, 0); // ENOSYS: the switch sees 255
    sys(1000, 5, 6, 7);      // ENOSYS
    sys(0, 5, 6, 7);
    g.add(g.make("ebreak", kEbreak));
    // Illegal encodings: compressed, unknown opcodes, reserved funct3 / funct7, CSR instructions.
    for (const u32 insn : {0x00000000u,
                           0xFFFFFFFFu,
                           0x00004501u,
                           0x0000000Bu,
                           0x0000007Fu,
                           0x00003003u,
                           0x00006003u,
                           0x00007003u,
                           0x00003023u,
                           0x00004023u,
                           0x00002063u,
                           0x00003063u,
                           0x00001067u,
                           0x02001013u,
                           0x40001013u,
                           0x02005013u,
                           0x80005013u,
                           0x40001033u,
                           0x02000033u | 0x04000000u,
                           0x80000033u,
                           0x40006033u,
                           0x30001073u,
                           0x00200073u,
                           0x00000173u,
                           0x10500073u}) {
        g.add(g.make("illegal", insn));
    }
    // Reads that reach the end of input come last: the VM's In answers EOF from then on.
    sys(63, 0, 0x407, 6, "abc");
    sys(63, 0, 0x400, 3, "");
}

std::string describe(const Case& c) {
    std::string s = c.what + " insn=" + hex(c.insn) + " pc=" + hex(c.pc);
    for (unsigned i = 1; i < 32; ++i) {
        s += " x" + std::to_string(i) + "=" + hex(c.x[i]);
    }
    return s + " brk=" + hex(c.brk) + " input=\"" + c.input + "\"";
}

// The first difference between the two states, or "".
std::string difference(const State& want, const State& got) {
    if (got.stop != want.stop) {
        return "stop " + std::to_string(got.stop) + ", want " + std::to_string(want.stop);
    }
    for (unsigned i = 1; i < 32; ++i) {
        if (got.x[i] != want.x[i]) {
            return "x" + std::to_string(i) + " = " + hex(got.x[i]) + ", want " + hex(want.x[i]);
        }
    }
    if (got.pc != want.pc) {
        return "pc = " + hex(got.pc) + ", want " + hex(want.pc);
    }
    if (got.brk != want.brk) {
        return "brk = " + hex(got.brk) + ", want " + hex(want.brk);
    }
    for (std::size_t i = 0; i < want.ram.size(); ++i) {
        if (got.ram[i] != want.ram[i]) {
            return "RAM word " + hex(static_cast<u32>(4 * i)) + " = " + hex(got.ram[i]) +
                   ", want " + hex(want.ram[i]);
        }
    }
    if (got.out != want.out) {
        return "output \"" + got.out + "\", want \"" + want.out + "\"";
    }
    if (got.in != want.in) {
        return "read " + std::to_string(got.in) + " input bytes, want " + std::to_string(want.in);
    }
    return "";
}

// Runs the cases on one emulator; returns the mean steps per class (of the cases that did not
// stop).
std::map<std::string, double> run_cases(const std::vector<Case>& cases) {
    Emu e(cases.front().ram);
    MESSAGE("startup: " << e.startup_steps << " steps; " << cases.size() << " cases");
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> cost; // class -> sum, count
    int failures = 0;
    for (const Case& c : cases) {
        const State want = reference(c);
        std::uint64_t steps = 0;
        const std::optional<State> got = e.run(c, steps);
        INFO(describe(c));
        REQUIRE_MESSAGE(got, "the emulator did not come back to fetch");
        const std::string diff = difference(want, *got);
        if (!diff.empty() && ++failures <= 20) {
            CHECK_MESSAGE(false, diff);
        }
        if (want.stop == 0) {
            const bool branch = (c.insn & 0x7F) == 0x63;
            auto& [sum, n] =
                cost[branch ? c.what + (want.pc == c.pc + 4 ? " (not taken)" : " (taken)")
                            : c.what];
            sum += steps;
            ++n;
        }
    }
    CHECK(failures == 0);
    std::map<std::string, double> mean;
    for (const auto& [what, sn] : cost) {
        mean[what] = static_cast<double>(sn.first) / static_cast<double>(sn.second);
    }
    return mean;
}

} // namespace

TEST_CASE("rv-emu: single instructions match the reference emulator, and their cost" *
          doctest::test_suite(kDebugBuild ? "slow" : "")) {
    Gen g(kRamPow2);
    // A debug build runs fewer cases (multiplication and division cost 15 to 20 others).
    alu_cases(g, kDebugBuild ? 4 : 12, kDebugBuild ? 2 : 8);
    memory_cases(g, kDebugBuild ? 12 : 40);
    control_cases(g, kDebugBuild ? 8 : 24);
    system_cases(g);
    const std::map<std::string, double> mean = run_cases(g.cases);

    // VM steps per instruction (docs/rv.md 8 records them).
    std::string table;
    for (const auto& [what, steps] : mean) {
        char line[64];
        std::snprintf(line, sizeof line, "%-16s %8.0f\n", what.c_str(), steps);
        table += line;
    }
    MESSAGE("steps per instruction:\n" << table);
}

// The paths that depend on the RAM size: the range checks and pc += 4 at the end of RAM. Not in a
// debug build, which would compile another emulator.
TEST_CASE("rv-emu: single instructions, guest RAM not a power of two" *
          doctest::test_suite("slow") * doctest::skip(kDebugBuild)) {
    Gen g(kRamOdd);
    alu_cases(g, 2, 1);
    memory_cases(g, 40);
    control_cases(g, 24);
    system_cases(g);
    run_cases(g.cases);
}
