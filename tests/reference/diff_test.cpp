// Differential test: ClassicVm against Olmstead's reference interpreter, step by step.
//
// The reference is the pristine malbolge.c with the small build-time instrumentation of
// instrument.cmake (per-step hook, I/O hooks, cells entry point, and a report instead of its hang
// or out-of-bounds read; see reference.h). For random valid programs both machines run in
// lockstep (the reference's per-step hook steps ClassicVm and compares, so nothing is buffered)
// and after every step the harness compares the decoded instruction, A, C, D, the cell at
// D (Opr/Rot target) and the cell at C (the encrypted cell) as written by that step, and the
// number of output bytes. At the end it compares the output and all 59049 cells.
//
// Where the reference would hang (E_EXEC) or read xlat2 out of bounds (E_ENCRYPT), ClassicVm must
// report that error at the same step with the same registers and memory (spec 8).
//
// MALM_DIFF_PROGRAMS and MALM_DIFF_STEPS (compile definitions) select the size: the fast test and
// the nightly slow test are the same source built twice. The slow build also defines
// MALM_DIFF_BEER99 (path) and runs the whole 99 bottles program in lockstep: 13.8 million steps,
// during which C and D wrap around 59049 tens of thousands of times, which random programs of
// 10,000 steps never reach.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/tables.hpp"
#include "core/vm.hpp"
#include "reference.h"

#ifndef MALM_DIFF_PROGRAMS
#error "MALM_DIFF_PROGRAMS not defined"
#endif
#ifndef MALM_DIFF_STEPS
#error "MALM_DIFF_STEPS not defined"
#endif

namespace {

using namespace malm::core;

constexpr unsigned kCells = 59049;

// Cells written by a step, from the registers the hook reports: after a completed step C and D
// were already incremented; for a halt or a fault they were not.
struct Written {
    unsigned wc, wd;
};
Written written_cells(int kind, unsigned c, unsigned d) {
    if (kind == REF_STEP) {
        return {(c + kCells - 1) % kCells, (d + kCells - 1) % kCells};
    }
    return {c, d};
}

struct StrIo : Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int read_byte() override {
        return pos >= in.size() ? kEof : static_cast<unsigned char>(in[pos++]);
    }
    bool write_byte(std::uint8_t b) override {
        out.push_back(static_cast<char>(b));
        return true;
    }
    bool flush() override { return true; }
};

// State of one lockstep run, reached from the extern "C" hooks. The reference's hook runs one
// ClassicVm step and compares; after the first mismatch the hooks do nothing.
struct Lockstep {
    std::string in; // reference stdin
    std::size_t in_pos = 0;
    std::string out; // reference output
    ClassicVm* vm = nullptr;
    StrIo* io = nullptr;
    std::string mismatch;
    std::size_t steps = 0; // events seen, including the terminal one
    int last_kind = -1;
};
Lockstep* g_ref = nullptr;

void on_ref_step(int kind, int op, unsigned a, unsigned c, unsigned d, const unsigned short* mem);

} // namespace

extern "C" {
void ref_hook(int kind, int op, unsigned a, unsigned c, unsigned d, const unsigned short* mem) {
    on_ref_step(kind, op, a, c, d, mem);
}
int ref_getc(void) {
    if (g_ref->in_pos >= g_ref->in.size()) {
        return -1;
    }
    return static_cast<unsigned char>(g_ref->in[g_ref->in_pos++]);
}
void ref_putc(int c) { g_ref->out.push_back(static_cast<char>(static_cast<unsigned char>(c))); }
}

namespace {

char op_letter(Op op) {
    switch (op) {
    case Op::Jmp:
        return 'i';
    case Op::Out:
        return '<';
    case Op::In:
        return '/';
    case Op::Rot:
        return '*';
    case Op::MovD:
        return 'j';
    case Op::Opr:
        return 'p';
    case Op::Nop:
        return 'o';
    case Op::Hlt:
        return 'v';
    }
    return '?';
}

// The reference reports the xlat1 letter; every letter that is not an instruction is a no-op.
char ref_letter(char op) {
    return std::string_view("ji*p</v").find(op) != std::string_view::npos ? op : 'o';
}

// Portable deterministic randomness (std distributions differ between standard libraries).
struct Rng {
    std::mt19937_64 gen;
    explicit Rng(std::uint64_t seed) : gen(seed) {}
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(gen() % n); }
};

struct Program {
    std::vector<std::uint8_t> cells;
    std::string input;
};

// Known programs with real loops (esolangs.org wiki, CC0): the non-halting cat, the truth-machine
// and hello-world. Random programs almost never loop, so half of the programs are mutants of
// these (a few cells replaced by other loadable characters, optionally a random tail appended).
constexpr std::string_view kBasePrograms[] = {
    R"mb((=BA#9"=<;:3y7x54-21q/p-,+*)"!h%B0/.~P<<:(8&66#"!~}|{zyxwvugJ%)mb",
    R"mb((aONMLKJIHGFEDCBA@?>=<;:98765FD21dd!-,O*)y'&v5#"!DC|Qzf,*vutsrqpF!Clk|ihgfed9(T&6KoOHZYXWVUTSRQPONM]KJIHGFEDCBA@?>=<;:9876"'~g|edybav_zyxwvotsrqpSnPlOjibKfedcba`_XA??ZYRW:UTSLQ3ONMLK.IHGFE>CBA@?"=<;:38765432s0/.n,+*)j!&%f{"!~}|_zyxZvYnsrqpRnmlkjML:f_^GF!)mb",
    R"mb((=<`#9]~6ZY32Vw/.R,+Op(L,+k#Gh&}Cdz@aw=;zyKw%ut4Uqp0/mlejihtfrHcbaC2^W\>Z,XW)UTSL53\HGFjW)mb",
};

// A random source character that is loadable at position `pos` (spec 4).
std::uint8_t random_loadable(Rng& rng, std::uint32_t pos) {
    std::vector<std::uint8_t> candidates;
    for (std::uint32_t ch = 33; ch <= 126; ++ch) {
        if (is_loadable(pos % 94, ch)) {
            candidates.push_back(static_cast<std::uint8_t>(ch));
        }
    }
    return candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
}

Program mutant_program(Rng& rng) {
    const std::string_view text = kBasePrograms[rng.below(std::size(kBasePrograms))];
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    const auto loaded = load_text(bytes, kCells);
    REQUIRE(loaded.has_value());
    Program p;
    p.cells = loaded.value();
    const std::uint32_t mutations = rng.below(2) == 0 ? 0 : 1 + rng.below(3);
    for (std::uint32_t i = 0; i < mutations; ++i) {
        const std::uint32_t pos = rng.below(static_cast<std::uint32_t>(p.cells.size()));
        p.cells[pos] = random_loadable(rng, pos);
    }
    const std::uint32_t tail = rng.below(2) == 0 ? 0 : rng.below(100);
    for (std::uint32_t i = 0; i < tail; ++i) {
        p.cells.push_back(random_loadable(rng, static_cast<std::uint32_t>(p.cells.size())));
    }
    const std::uint32_t in_len = rng.below(48);
    for (std::uint32_t i = 0; i < in_len; ++i) {
        p.input.push_back(
            static_cast<char>(rng.below(2) == 0 ? '0' + rng.below(2) : rng.below(256)));
    }
    return p;
}

// A random loadable program: cell i holds a source character that decodes (spec 4) to a random
// instruction letter at position i. Each program draws its own instruction mix, because a uniform
// mix ends within a few dozen steps (an early Jmp lands in the pseudo-random fill, Opr/Rot at
// D == C rewrite the cell about to be encrypted, Hlt is frequent). The mixes cover straight-line
// code, loops in the low cells, programs that write memory and programs that halt.
Program random_program(Rng& rng, unsigned index) {
    if (index % 2 == 1) {
        return mutant_program(rng);
    }
    static constexpr unsigned kJmpWeight[] = {0, 0, 1, 3, 10};     // `i`
    static constexpr unsigned kHeavyWeight[] = {0, 0, 0, 1, 3, 8}; // `*` and `p`, each
    static constexpr unsigned kHaltWeight[] = {0, 0, 0, 1, 8};     // `v`, out of 1024
    const unsigned jmp = kJmpWeight[rng.below(5)];
    const unsigned heavy = kHeavyWeight[rng.below(6)];
    const unsigned halt_w = kHaltWeight[rng.below(5)];
    const std::uint32_t len = rng.below(2) == 0 ? 2 + rng.below(1999) : 1000 + rng.below(1001);
    Program p;
    p.cells.reserve(len);
    for (std::uint32_t i = 0; i < len; ++i) {
        char letter;
        if (rng.below(1024) < halt_w) {
            letter = 'v';
        } else {
            // weights: j < / o = 8 each, i = `jmp`, * and p = `heavy` each
            constexpr std::string_view kLight = "j</o";
            const std::uint32_t r = rng.below(4 * 8 + jmp + 2 * heavy);
            letter = r < 32 ? kLight[r / 8] : r < 32 + jmp ? 'i' : r < 32 + jmp + heavy ? '*' : 'p';
        }
        // Source characters whose decode at position i is `letter` (spec 4, loadable cells).
        std::vector<std::uint8_t> candidates;
        for (std::uint32_t ch = 33; ch <= 126; ++ch) {
            if (kXlat1[(ch - 33 + i) % 94] == letter) {
                candidates.push_back(static_cast<std::uint8_t>(ch));
            }
        }
        p.cells.push_back(candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))]);
    }
    const std::uint32_t in_len = index % 5 == 0 ? 0 : rng.below(48);
    for (std::uint32_t i = 0; i < in_len; ++i) {
        p.input.push_back(static_cast<char>(rng.below(256)));
    }
    return p;
}

enum class End { Halted, ExecFault, EncryptFault, StepCap };

struct Verdict {
    std::string mismatch; // empty if both machines agree
    End end = End::StepCap;
    std::size_t steps = 0;
};

std::string hex(unsigned v) { return std::to_string(v); }

// One reference step (called from ref_hook): steps ClassicVm and compares everything.
void on_ref_step(int kind, int op, unsigned a, unsigned c, unsigned d, const unsigned short* mem) {
    Lockstep& ls = *g_ref;
    if (!ls.mismatch.empty()) {
        return;
    }
    ClassicVm& vm = *ls.vm;
    const std::size_t i = ls.steps++;
    ls.last_kind = kind;
    const auto fail = [&](const std::string& what) {
        ls.mismatch = "step " + std::to_string(i + 1) + ": " + what;
    };
    const ClassicWord c_before = vm.c();
    const ClassicWord cell_before = vm.memory().read(c_before);
    std::optional<Op> op_before;
    if (ClassicMachine::is_cell(cell_before)) {
        op_before = decode(c_before % 94u, cell_before);
    }
    const StepResult r = vm.step(*ls.io);

    int vm_kind = -1;
    switch (r.status) {
    case Status::Continue:
        vm_kind = REF_STEP;
        break;
    case Status::Halted:
        vm_kind = REF_HALT;
        break;
    case Status::Faulted:
        vm_kind = r.fault->error == Error::Exec      ? REF_E_EXEC
                  : r.fault->error == Error::Encrypt ? REF_E_ENCRYPT
                                                     : -1;
        break;
    }
    if (vm_kind != kind) {
        return fail("outcome: reference kind " + std::to_string(kind) + ", VM kind " +
                    std::to_string(vm_kind));
    }
    if (kind == REF_STEP || kind == REF_HALT) {
        if (!op_before || op_letter(*op_before) != ref_letter(static_cast<char>(op))) {
            return fail(std::string("instruction: reference '") +
                        ref_letter(static_cast<char>(op)) + "'");
        }
    }
    if (vm.a() != a || vm.c() != c || vm.d() != d) {
        return fail("registers: reference A=" + hex(a) + " C=" + hex(c) + " D=" + hex(d) +
                    ", VM A=" + hex(vm.a()) + " C=" + hex(vm.c()) + " D=" + hex(vm.d()));
    }
    if (r.fault && r.fault->steps != i + 1) {
        return fail("fault step number " + std::to_string(r.fault->steps));
    }
    const Written w = written_cells(kind, c, d);
    const ClassicWord vc = vm.memory().read(static_cast<ClassicWord>(w.wc));
    const ClassicWord vd = vm.memory().read(static_cast<ClassicWord>(w.wd));
    if (vc != mem[w.wc] || vd != mem[w.wd]) {
        return fail("written cells: reference [" + hex(w.wc) + "]=" + hex(mem[w.wc]) + " [" +
                    hex(w.wd) + "]=" + hex(mem[w.wd]) + ", VM " + hex(vc) + " " + hex(vd));
    }
    // Output so far: the length every step; the bytes are compared at the end.
    if (ls.io->out.size() != ls.out.size()) {
        return fail("output length: reference " + std::to_string(ls.out.size()) + ", VM " +
                    std::to_string(ls.io->out.size()));
    }
}

// Runs the reference and ClassicVm on `p` for up to `max_steps` steps and compares them.
Verdict compare(const Program& p, std::uint64_t max_steps) {
    Lockstep ls;
    ls.in = p.input;
    ClassicVm vm(p.cells);
    StrIo io;
    io.in = p.input;
    ls.vm = &vm;
    ls.io = &io;
    g_ref = &ls;
    ref_run(p.cells.data(), p.cells.size(), max_steps);
    g_ref = nullptr;

    Verdict v;
    v.steps = ls.steps;
    v.mismatch = ls.mismatch;
    const auto fail = [&](const std::string& what) {
        v.mismatch = "step " + std::to_string(ls.steps) + ": " + what;
        return v;
    };
    if (!v.mismatch.empty()) {
        return v;
    }
    if (ls.last_kind == REF_STEP || ls.steps == 0) {
        if (ls.steps != max_steps) {
            return fail("reference stopped without a terminal event");
        }
        v.end = End::StepCap;
    } else {
        v.end = ls.last_kind == REF_HALT     ? End::Halted
                : ls.last_kind == REF_E_EXEC ? End::ExecFault
                                             : End::EncryptFault;
    }
    if (io.out != ls.out) {
        return fail("final output differs");
    }
    const unsigned short* rm = ref_memory();
    const auto cells = vm.memory().cells();
    for (unsigned i = 0; i < kCells; ++i) {
        if (cells[i] != rm[i]) {
            return fail("final memory [" + std::to_string(i) + "]: reference " + hex(rm[i]) +
                        ", VM " + hex(cells[i]));
        }
    }
    return v;
}

} // namespace

TEST_CASE("classic VM agrees with the reference interpreter step by step") {
    constexpr unsigned kPrograms = MALM_DIFF_PROGRAMS;
    constexpr std::uint64_t kSteps = MALM_DIFF_STEPS;
    Rng rng(0x4d414c42304c4745ull); // fixed seed
    unsigned counts[4] = {};
    std::uint64_t total_steps = 0;
    for (unsigned i = 0; i < kPrograms; ++i) {
        const Program p = random_program(rng, i);
        const Verdict v = compare(p, kSteps);
        CHECK_MESSAGE(v.mismatch.empty(),
                      "program " << i << " (" << p.cells.size() << " cells): " << v.mismatch);
        ++counts[static_cast<int>(v.end)];
        total_steps += v.steps;
    }
    MESSAGE(kPrograms << " programs x " << kSteps << " steps: halted " << counts[0] << ", E_EXEC "
                      << counts[1] << ", E_ENCRYPT " << counts[2] << ", step cap " << counts[3]
                      << "; " << total_steps << " steps compared");
    // Guard against a generator that stopped covering the outcomes.
    CHECK(counts[0] > 0);
    CHECK(counts[1] > 0);
    CHECK(counts[2] > 0);
    CHECK(counts[3] > 0);
}

#ifdef MALM_DIFF_BEER99
TEST_CASE("beer99 runs in lockstep with the reference (C and D wrap)") {
    std::ifstream f(MALM_DIFF_BEER99, std::ios::binary);
    REQUIRE(f.good());
    const std::vector<std::uint8_t> text{std::istreambuf_iterator<char>(f),
                                         std::istreambuf_iterator<char>()};
    const auto loaded = load_text(text, kCells);
    REQUIRE(loaded.has_value());
    Program p;
    p.cells = loaded.value();
    const Verdict v = compare(p, 20'000'000);
    CHECK_MESSAGE(v.mismatch.empty(), v.mismatch);
    CHECK(v.end == End::Halted);
    CHECK(v.steps == 13802606);
    MESSAGE("beer99: " << v.steps << " steps compared");
}
#endif
