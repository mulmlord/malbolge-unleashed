#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asm/cycles.hpp"
#include "core/classic.hpp"
#include "core/tables.hpp"
#include "core/vm.hpp"

using namespace malm::assembler;
using malm::core::kXlat2;

namespace {

constexpr std::uint8_t kF = 70;
constexpr std::uint8_t kJ = 74;

// True if `a` and `b` are the same cycle up to rotation.
bool same_cycle(std::string_view a, std::string_view b) {
    return a.size() == b.size() && (std::string(a) + std::string(a)).find(b) != std::string::npos;
}

std::set<std::size_t> to_set(std::initializer_list<std::size_t> l) { return {l}; }

std::string allowed_at(std::size_t p) {
    std::string out;
    for (unsigned c = 33; c <= 126; ++c) {
        if (malm::core::is_loadable(static_cast<std::uint32_t>(p), c)) {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// Positions where op has a 2-state (F/J) flag; research 1.6 lists these pairs.
std::set<std::size_t> flag_positions(Op op) {
    std::set<std::size_t> s;
    for (std::size_t p = 0; p < 94; ++p) {
        if (flag_chars(p, op).count != 0) {
            s.insert(p);
        }
    }
    return s;
}

} // namespace

TEST_CASE("xlat2 cycle structure") {
    std::vector<std::size_t> lengths;
    for (std::size_t id = 0; id < kNumCycles; ++id) {
        lengths.push_back(cycle_members(id).size());
    }
    CHECK(lengths == std::vector<std::size_t>{2, 4, 5, 6, 9, 68});

    // Research 1.6: the short cycles.
    CHECK(same_cycle(cycle_members(0), "FJ"));
    CHECK(same_cycle(cycle_members(1), "*r}i"));
    CHECK(same_cycle(cycle_members(2), ")f`<3"));
    CHECK(same_cycle(cycle_members(3), "%guox:"));
    CHECK(same_cycle(cycle_members(4), "2PB>LOCUI"));

    std::array<bool, 128> seen{};
    for (unsigned c = 33; c <= 126; ++c) {
        const auto ch = static_cast<std::uint8_t>(c);
        const std::string_view m = cycle_members(cycle_id(ch));
        CHECK(m[cycle_index(ch)] == static_cast<char>(c));
        CHECK(cycle_length(ch) == m.size());
        CHECK(cycle_next(ch) == static_cast<unsigned char>(kXlat2[c - 33]));
        CHECK(cycle_next(ch, m.size()) == ch);
        // members are in cycle order
        for (std::size_t i = 0; i < m.size(); ++i) {
            CHECK(static_cast<unsigned char>(m[(i + 1) % m.size()]) ==
                  static_cast<unsigned char>(kXlat2[static_cast<unsigned char>(m[i]) - 33]));
        }
        seen[c] = true;
    }
    CHECK(std::count(seen.begin() + 33, seen.begin() + 127, true) == 94);
}

TEST_CASE("the only 2-cycle is F <-> J, address independent") {
    CHECK(cycle_length(kF) == 2);
    CHECK(cycle_length(kJ) == 2);
    CHECK(cycle_next(kF) == kJ);
    CHECK(cycle_next(kJ) == kF);
    int twos = 0;
    for (unsigned c = 33; c <= 126; ++c) {
        twos += cycle_length(static_cast<std::uint8_t>(c)) == 2 ? 1 : 0;
    }
    CHECK(twos == 2);
}

TEST_CASE("FJ positions where one element is an instruction and the other a Nop") {
    struct Row {
        Op op;
        std::size_t a;
        std::size_t b;
    };
    const Row rows[] = {{Op::Out, 25, 29}, {Op::In, 43, 47},  {Op::Rot, 59, 63}, {Op::MovD, 60, 64},
                        {Op::Opr, 82, 86}, {Op::Jmp, 24, 28}, {Op::Hlt, 7, 11}};
    for (const Row& r : rows) {
        CAPTURE(static_cast<int>(r.op));
        CHECK(flag_positions(r.op) == to_set({r.a, r.b}));
        // At one position of the pair F is the instruction and J the Nop, at the other the reverse.
        for (const std::size_t p : {r.a, r.b}) {
            const bool f_is_op = op_at(p, kF, 0) == r.op;
            CHECK(op_at(p, kF, 0) == (f_is_op ? r.op : Op::Nop));
            CHECK(op_at(p, kJ, 0) == (f_is_op ? Op::Nop : r.op));
        }
        CHECK(op_at(r.a, kF, 0) != op_at(r.b, kF, 0));
    }
    // 'o' is the Nop instruction itself: at 88 / 92 the loadable 'o' is J / F and both elements
    // decode to Nop (an immutable Nop pair, research 1.6).
    for (const std::size_t p : {88u, 92u}) {
        CHECK((op_at(p, kF, 0) == Op::Nop && op_at(p, kJ, 0) == Op::Nop));
        CHECK(loadable_char(p, Op::Nop) == (p == 88 ? kJ : kF));
    }
}

TEST_CASE("immutable Nops") {
    std::set<std::size_t> o_positions;
    for (std::size_t p = 0; p < 94; ++p) {
        const auto c = immutable_nop_char(p);
        if (c) {
            o_positions.insert(p);
            CHECK(*c == loadable_char(p, Op::Nop));
            CHECK(is_immutable_nop(p, *c));
            CHECK(malm::core::is_loadable(static_cast<std::uint32_t>(p), *c));
        }
    }
    CHECK(o_positions == to_set({6, 8, 10, 17, 26, 27, 37, 42, 48, 51, 82, 86, 88, 92}));

    std::set<std::size_t> mutable_fj;
    for (std::size_t p = 0; p < 94; ++p) {
        CHECK(is_immutable_nop(p, kF) == is_immutable_nop(p, kJ));
        if (!is_immutable_nop(p, kF)) {
            mutable_fj.insert(p);
        }
    }
    CHECK(mutable_fj == to_set({7, 11, 24, 25, 28, 29, 43, 47, 59, 60, 63, 64, 82, 86}));

    // Definition cross-check against the sequences.
    for (std::size_t p = 0; p < 94; ++p) {
        for (unsigned c = 33; c <= 126; ++c) {
            const auto ch = static_cast<std::uint8_t>(c);
            const OpSequence s = op_sequence(p, ch);
            const bool all_nop =
                std::all_of(s.ops.begin(), s.ops.begin() + static_cast<std::ptrdiff_t>(s.length),
                            [](Op o) { return o == Op::Nop; });
            CHECK(is_immutable_nop(p, ch) == all_nop);
        }
    }

    // RNop exists everywhere, including the sled residues 14..16 (...20, ...21, ...2).
    for (std::size_t p = 0; p < 94; ++p) {
        const std::uint8_t r = rnop_char(p);
        REQUIRE(r >= 33);
        CHECK(is_immutable_nop(p, r));
        if (immutable_nop_char(p)) {
            CHECK(r == *immutable_nop_char(p));
        }
    }
}

TEST_CASE("allowed characters per position") {
    CHECK(allowed_at(0) == "'(>DQbcu");
    CHECK(allowed_at(1) == "&'=CPabt");
    for (std::size_t p = 0; p < 94; ++p) {
        std::string placed;
        for (const Op op :
             {Op::Jmp, Op::Out, Op::In, Op::Rot, Op::MovD, Op::Opr, Op::Nop, Op::Hlt}) {
            const std::uint8_t c = loadable_char(p, op);
            CHECK(malm::core::is_loadable(static_cast<std::uint32_t>(p), c));
            CHECK(op_at(p, c, 0) == op);
            placed += static_cast<char>(c);
        }
        std::sort(placed.begin(), placed.end());
        CHECK(placed == allowed_at(p));
    }
}

TEST_CASE("flag candidates") {
    for (std::size_t p = 0; p < 94; ++p) {
        for (const std::size_t len : {2u, 4u, 5u, 6u, 9u}) {
            for (const Op op : {Op::Jmp, Op::Out, Op::In, Op::Rot, Op::MovD, Op::Opr, Op::Hlt}) {
                const FlagCandidates fc = flag_candidates(p, op, len);
                CHECK((fc.count == 0 || fc.count == len));
                for (std::size_t i = 0; i < fc.count; ++i) {
                    const FlagCandidate& f = fc.items[i];
                    CHECK(f.length == len);
                    CHECK(cycle_length(f.ch) == len);
                    bool has_op = false;
                    bool has_nop = false;
                    for (std::size_t k = 0; k < len; ++k) {
                        CHECK(f.ops[k] == op_at(p, f.ch, k));
                        has_op = has_op || f.ops[k] == op;
                        has_nop = has_nop || f.ops[k] == Op::Nop;
                    }
                    CHECK(has_op);
                    CHECK(has_nop);
                    CHECK(is_reachable(p, f.ch));
                }
            }
        }
    }
    // Two-state flags equal the L == 2 candidates, and are F / J only.
    const FlagCandidates fc = flag_chars(25, Op::Out);
    REQUIRE(fc.count == 2);
    CHECK(((fc.items[0].ch == kF && fc.items[1].ch == kJ) ||
           (fc.items[0].ch == kJ && fc.items[1].ch == kF)));
    CHECK(flag_chars(0, Op::Out).count == 0);
    CHECK(flag_candidates(25, Op::Nop, 2).count == 0);
    CHECK(flag_candidates(25, Op::Out, 3).count == 0);
}

// Cross-check against the real VM (classic mode, which needs no big-number setup): put a loadable
// char at an address congruent to p, execute it repeatedly, classify the instruction from its
// effects, and compare with op_at and with the encryption step.
namespace {

struct RecIo : malm::core::Io {
    std::size_t out = 0;
    std::size_t reads = 0;
    int read_byte() override {
        ++reads;
        return 0;
    }
    bool write_byte(std::uint8_t) override {
        ++out;
        return true;
    }
    bool flush() override { return true; }
};

} // namespace

// Checks the positions p with p % stride == 0 (a stride of 1 checks all 94); every op, both
// addresses and every cycle step are still checked at each of them.
static void check_tables_against_vm(std::size_t stride) {
    using malm::core::ClassicVm;
    using malm::core::ClassicWord;
    using malm::core::Status;
    constexpr std::size_t kLen = 400;
    constexpr ClassicWord kD0 = 350;
    constexpr std::uint8_t kOperand =
        100; // cell value at kD0; below both test addresses' resume points

    for (std::size_t p = 0; p < 94; ++p) {
        if (p % stride != 0) {
            continue;
        }
        for (const std::size_t addr : {p, p + 188}) {
            for (const Op want :
                 {Op::Jmp, Op::Out, Op::In, Op::Rot, Op::MovD, Op::Opr, Op::Nop, Op::Hlt}) {
                const std::uint8_t c0 = loadable_char(p, want);
                std::uint8_t cell = c0; // the cell as the VM holds it, carried between steps
                const std::size_t len = cycle_length(c0);
                for (std::size_t k = 0; k < len; ++k) {
                    CAPTURE(p);
                    CAPTURE(addr);
                    CAPTURE(k);
                    std::vector<std::uint8_t> prog(kLen, '!');
                    prog[addr] = cell;
                    prog[kD0] = kOperand;
                    prog[kOperand] = 'd';
                    ClassicVm vm(prog);
                    vm.set_registers({7, static_cast<ClassicWord>(addr), kD0});
                    RecIo io;
                    const Status st = vm.step(io).status;
                    // Classify by effect.
                    Op got = Op::Nop;
                    if (st == Status::Halted) {
                        got = Op::Hlt;
                    } else if (io.out != 0) {
                        got = Op::Out;
                    } else if (io.reads != 0) {
                        got = Op::In;
                    } else if (vm.c() != addr + 1) {
                        got = Op::Jmp;
                    } else if (vm.d() != kD0 + 1) {
                        got = Op::MovD;
                    } else if (vm.memory().read(kD0) != kOperand) {
                        got = (vm.a() == malm::core::classic_rotr(kOperand)) ? Op::Rot : Op::Opr;
                    }
                    CHECK(got == op_at(p, c0, k));
                    if (got == Op::Hlt || got == Op::Jmp) {
                        break; // Hlt stops; a Jmp is never encrypted, so the cell stays put
                    }
                    // Encryption step: the cell advanced by one xlat2 step.
                    cell =
                        static_cast<std::uint8_t>(vm.memory().read(static_cast<ClassicWord>(addr)));
                    CHECK(cell == cycle_next(static_cast<std::uint8_t>(prog[addr])));
                }
            }
        }
    }
}

// Fast: a fixed sample of positions. The full sweep runs in the slow suite.
TEST_CASE("tables agree with a real VM (sampled positions)") { check_tables_against_vm(13); }

TEST_CASE("tables agree with a real VM (all positions)" * doctest::test_suite("slow")) {
    check_tables_against_vm(1);
}
