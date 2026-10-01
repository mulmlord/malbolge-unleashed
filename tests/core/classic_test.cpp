#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/ops.hpp"
#include "core/tables.hpp"
#include "core/vm.hpp"

namespace {

using namespace malm::core;

// Reference interpreter op() on 10-trit words, spec 3.2 di-trit table.
std::uint32_t classic_op(std::uint32_t a, std::uint32_t d) {
    static constexpr unsigned o[9][9] = {
        {4, 3, 3, 1, 0, 0, 1, 0, 0}, {4, 3, 5, 1, 0, 2, 1, 0, 2}, {5, 5, 4, 2, 2, 1, 2, 2, 1},
        {4, 3, 3, 1, 0, 0, 7, 6, 6}, {4, 3, 5, 1, 0, 2, 7, 6, 8}, {5, 5, 4, 2, 2, 1, 8, 8, 7},
        {7, 6, 6, 7, 6, 6, 4, 3, 3}, {7, 6, 8, 7, 6, 8, 4, 3, 5}, {8, 8, 7, 8, 8, 7, 5, 5, 4}};
    std::uint32_t r = 0;
    for (const std::uint32_t p9 : {1u, 9u, 81u, 729u, 6561u}) {
        r += o[d / p9 % 9][a / p9 % 9] * p9;
    }
    return r;
}

// String-backed Io with failure injection.
struct StrIo : Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    bool fail_read = false;
    bool fail_write = false;
    bool fail_flush = false;

    int read_byte() override {
        if (fail_read) {
            return kIoError;
        }
        if (pos >= in.size()) {
            return kEof;
        }
        return static_cast<unsigned char>(in[pos++]);
    }
    bool write_byte(std::uint8_t b) override {
        if (fail_write) {
            return false;
        }
        out.push_back(static_cast<char>(b));
        return true;
    }
    bool flush() override { return !fail_flush; }
};

std::vector<std::uint8_t> classic_cells(const std::string& text) {
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    auto r = load_text(bytes, kClassicCells);
    REQUIRE(r.has_value());
    return *std::move(r);
}

// Random program: every cell a valid instruction at its position (loader rule).
std::vector<std::uint8_t> random_program(std::mt19937_64& rng, std::size_t len) {
    std::vector<std::uint8_t> p(len);
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t b = 0;
        do {
            b = static_cast<std::uint8_t>(33 + rng() % 94);
        } while (!is_loadable(static_cast<std::uint32_t>(i % 94), b));
        p[i] = b;
    }
    return p;
}

// Cells of a program given as instruction letters (see vm_test.cpp).
std::vector<std::uint8_t> prog(const std::string& ops) {
    std::vector<std::uint8_t> out;
    for (std::size_t p = 0; p < ops.size(); ++p) {
        for (std::uint32_t code = 33; code <= 126; ++code) {
            if (kXlat1[(code - 33 + p) % 94] == ops[p]) {
                out.push_back(static_cast<std::uint8_t>(code));
                break;
            }
        }
    }
    REQUIRE(out.size() == ops.size());
    return out;
}

Value nat(std::uint64_t k) { return *Value::natural(k); }

} // namespace

// ---- classic ops ---------------------------------------------------------------------------

TEST_CASE("classic_crz matches the di-trit op() on random pairs") {
    std::mt19937_64 rng(7);
    for (int i = 0; i < 1'200'000; ++i) {
        const auto a = static_cast<ClassicWord>(rng() % kClassicCells);
        const auto d = static_cast<ClassicWord>(rng() % kClassicCells);
        if (classic_crz(a, d) != classic_op(a, d)) {
            FAIL("a=" << a << " d=" << d);
        }
    }
}

TEST_CASE("classic_crz matches op() on every pair involving an edge value") {
    const ClassicWord edges[] = {0, 1, 2, 59046, 59047, 59048};
    for (const ClassicWord e : edges) {
        for (std::uint32_t x = 0; x < kClassicCells; ++x) {
            const auto w = static_cast<ClassicWord>(x);
            if (classic_crz(e, w) != classic_op(e, w) || classic_crz(w, e) != classic_op(w, e)) {
                FAIL("e=" << e << " x=" << x);
            }
        }
    }
}

TEST_CASE("classic_crz equals the low 10 trits of the unleashed crz on naturals") {
    std::mt19937_64 rng(8);
    for (int i = 0; i < 200'000; ++i) {
        const auto a = static_cast<ClassicWord>(rng() % kClassicCells);
        const auto d = static_cast<ClassicWord>(rng() % kClassicCells);
        if (classic_crz(a, d) != crz(nat(a), nat(d)).raw() % kClassicCells) {
            FAIL("a=" << a << " d=" << d);
        }
    }
}

TEST_CASE("classic_crz never leaves 0..59048") {
    CHECK(classic_crz(0, 0) == 29524); // 10 trits of 1
    CHECK(classic_crz(59048, 59048) == 29524);
    CHECK(classic_crz(59048, 0) == 0);
    CHECK(classic_crz(0, 59048) == 59048);
}

TEST_CASE("classic_rotr on all 59049 values") {
    for (std::uint32_t v = 0; v < kClassicCells; ++v) {
        const std::uint32_t expect = v / 3 + (v % 3) * 19683;
        if (classic_rotr(static_cast<ClassicWord>(v)) != expect) {
            FAIL("v=" << v);
        }
    }
    CHECK(classic_rotr(1) == 19683);
    CHECK(classic_rotr(59048) == 59048);
}

TEST_CASE("classic increment wraps at 59048") {
    CHECK(classic_inc(0) == 1);
    CHECK(classic_inc(59047) == 59048);
    CHECK(classic_inc(59048) == 0);
}

// ---- classic memory ------------------------------------------------------------------------

TEST_CASE("classic memory fill equals the literal recurrence") {
    std::mt19937_64 rng(11);
    auto check = [](const std::vector<std::uint8_t>& p) {
        const ClassicMemory m(p);
        std::vector<std::uint32_t> f(kClassicCells);
        for (std::size_t i = 0; i < p.size(); ++i) {
            f[i] = p[i];
        }
        for (std::size_t i = p.size(); i < kClassicCells; ++i) {
            f[i] = classic_op(f[i - 1], f[i - 2]);
        }
        REQUIRE(m.cells().size() == kClassicCells);
        for (std::size_t i = 0; i < kClassicCells; ++i) {
            if (m.read(static_cast<ClassicWord>(i)) != f[i]) {
                FAIL("len=" << p.size() << " i=" << i);
            }
        }
    };
    for (int iter = 0; iter < 30; ++iter) {
        check(random_program(rng, 2 + rng() % 3000));
    }
    for (const std::size_t len : {2u, 3u, 59047u, 59048u, 59049u}) {
        check(random_program(rng, len));
    }
}

TEST_CASE("classic memory: a full-length program has no fill") {
    std::mt19937_64 rng(12);
    const auto p = random_program(rng, kClassicCells);
    const ClassicMemory m(p);
    for (std::size_t i = 0; i < kClassicCells; ++i) {
        REQUIRE(m.read(static_cast<ClassicWord>(i)) == p[i]);
    }
}

// ---- loader limit --------------------------------------------------------------------------

TEST_CASE("classic loader accepts 59049 cells and rejects 59050 at the offset of cell 59049") {
    std::string ok(59049, 'o');
    std::string text;
    for (const std::uint8_t b : prog(ok)) {
        text.push_back(static_cast<char>(b));
    }
    CHECK(classic_cells(text).size() == 59049);

    std::string over =
        text + std::string(1, static_cast<char>(prog(std::string(59050, 'o')).back()));
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(over.data()),
                                              over.size());
    const auto r = load_text(bytes, kClassicCells);
    REQUIRE(!r.has_value());
    CHECK(r.error().offset == 59049);

    // The unleashed limit still accepts it.
    CHECK(load_text(bytes).has_value());

    // Whitespace before the offending character counts as skipped bytes.
    over.insert(59049, "\n ");
    const std::span<const std::uint8_t> bytes2(reinterpret_cast<const std::uint8_t*>(over.data()),
                                               over.size());
    const auto r2 = load_text(bytes2, kClassicCells);
    REQUIRE(!r2.has_value());
    CHECK(r2.error().offset == 59051);
}

// ---- ClassicVm -----------------------------------------------------------------------------

TEST_CASE("ClassicVm: step-by-step trace checked against the reference interpreter") {
    // Normalized "/jp*<ojv" (In MovD Opr Rot Out Nop MovD Hlt); values derived with the
    // reference interpreter model. Fill: [8] = 29546, [9] = 46, ...
    const auto cells = classic_cells("u'<$_?\"J");
    ClassicVm vm(cells);
    StrIo io;
    io.in = "Q";
    CHECK(vm.n() == 10);
    CHECK(vm.memory().read(8) == 29546);

    struct Row {
        ClassicWord a, c, d;
        std::vector<std::pair<ClassicWord, ClassicWord>> cells; // address -> value after the step
    };
    const std::vector<Row> rows = {
        {81, 1, 1, {{0, 111}}},
        {81, 2, 40, {{1, 116}}},
        {29474, 3, 41, {{40, 29474}, {2, 51}}},
        {39390, 4, 42, {{41, 39390}, {3, 38}}},
        {39390, 5, 43, {{4, 109}}},
        {39390, 6, 44, {{5, 43}}},
        {39390, 7, 29547, {{6, 122}}},
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CAPTURE(i + 1);
        const StepResult r = vm.step(io);
        CHECK(r.status == Status::Continue);
        CHECK(vm.steps() == i + 1);
        CHECK(vm.a() == rows[i].a);
        CHECK(vm.c() == rows[i].c);
        CHECK(vm.d() == rows[i].d);
        for (const auto& [addr, val] : rows[i].cells) {
            CHECK(vm.memory().read(addr) == val);
        }
    }
    CHECK(io.out == std::string(1, static_cast<char>(222))); // 39390 mod 256
    const StepResult last = vm.step(io);
    CHECK(last.status == Status::Halted);
    CHECK(vm.steps() == 8);
    CHECK(vm.a() == 39390);
    CHECK(vm.c() == 7);
    CHECK(vm.d() == 29547);
    CHECK(vm.n() == 10);
    CHECK(vm.halted());
}

TEST_CASE("ClassicVm: exit status") {
    ClassicVm vm(classic_cells("u'<$_?\"J"));
    StrIo io;
    io.in = "Q";
    const RunResult r = vm.run(io);
    CHECK(r.kind == RunResult::Kind::Halted);
    CHECK(exit_status(r, vm, false) == 0);
    CHECK(exit_status(r, vm, true) == 39390 % 256);
}

TEST_CASE("ClassicVm: E_EXEC") {
    ClassicVm vm(classic_cells("DCB")); // ooo, then fill cell 3 = 29512 is outside 33..126
    StrIo io;
    const RunResult r = vm.run(io);
    REQUIRE(r.kind == RunResult::Kind::Fault);
    CHECK(r.fault->error == Error::Exec);
    CHECK(r.fault->steps == 4);
    CHECK(r.fault->c == nat(3));
    CHECK(r.fault->d == nat(3));
    CHECK(r.fault->a == nat(0));
    CHECK(r.fault->n == 10);
    CHECK(format_fault(*r.fault) == "E_EXEC at step 4: C=0t10 D=0t10 A=0t0 n=10");
    CHECK(exit_status(r, vm, true) == 1);
}

TEST_CASE("ClassicVm: E_ENCRYPT") {
    ClassicVm vm(classic_cells("u=&$:^\"J")); // /pj*...: Opr writes a value outside 33..126 at C
    StrIo io;
    io.in = "Q";
    const RunResult r = vm.run(io);
    REQUIRE(r.kind == RunResult::Kind::Fault);
    CHECK(r.fault->error == Error::Encrypt);
    CHECK(r.fault->steps == 2);
    CHECK(r.fault->c == nat(1));
    CHECK(r.fault->d == nat(1));
    CHECK(r.fault->a == nat(29473));
    CHECK_FALSE(r.fault->overflow);
}

TEST_CASE("ClassicVm: E_IO on read, write and flush") {
    {
        ClassicVm vm(classic_cells("ubO")); // In Out Hlt
        StrIo io;
        io.fail_read = true;
        const RunResult r = vm.run(io);
        REQUIRE(r.kind == RunResult::Kind::Fault);
        CHECK(r.fault->error == Error::Io);
        CHECK(r.fault->steps == 1);
    }
    {
        ClassicVm vm(classic_cells("ubO"));
        StrIo io;
        io.in = "x";
        io.fail_write = true;
        const RunResult r = vm.run(io);
        REQUIRE(r.kind == RunResult::Kind::Fault);
        CHECK(r.fault->error == Error::Io);
        CHECK(r.fault->steps == 2);
    }
    {
        ClassicVm vm(classic_cells("ubO"));
        StrIo io;
        io.in = "x";
        io.fail_flush = true; // In flushes first
        const RunResult r = vm.run(io);
        REQUIRE(r.kind == RunResult::Kind::Fault);
        CHECK(r.fault->error == Error::Io);
        CHECK(r.fault->steps == 1);
    }
}

TEST_CASE("ClassicVm: EOF reads as 59048 and echoes as byte 168") {
    ClassicVm vm(classic_cells("ubO")); // In Out Hlt
    StrIo io;
    CHECK(vm.step(io).status == Status::Continue);
    CHECK(vm.a() == 59048);
    CHECK(vm.step(io).status == Status::Continue);
    CHECK(io.out == std::string(1, static_cast<char>(168)));
    CHECK(vm.step(io).status == Status::Halted);
}

TEST_CASE("ClassicVm: set_registers reduces out-of-range words mod 59049") {
    const auto p = prog(std::string(100, 'o'));
    ClassicVm vm(p);
    StrIo io;
    vm.set_registers(ClassicRegisters{.a = 65535, .c = 59049 + 5, .d = 65535});
    CHECK(vm.a() == 65535 % 59049);
    CHECK(vm.c() == 5);
    CHECK(vm.d() == 65535 % 59049);
    CHECK(vm.step(io).status == Status::Continue); // no out-of-bounds access (ASan)
    CHECK(vm.c() == 6);
    CHECK(vm.d() == 65535 % 59049 + 1);
}

TEST_CASE("ClassicMemory rejects program lengths outside 2..59049") {
    CHECK_THROWS_AS(ClassicMemory(std::vector<std::uint8_t>(kClassicCells + 1, 'o')),
                    std::length_error);
    CHECK_THROWS_AS(ClassicMemory(std::vector<std::uint8_t>(1, 'o')), std::length_error);
    CHECK_THROWS_AS(ClassicMemory(std::vector<std::uint8_t>{}), std::length_error);
    CHECK_NOTHROW(ClassicMemory(std::vector<std::uint8_t>(kClassicCells, 'o')));
}

TEST_CASE("ClassicVm: C and D wrap from 59048 to 0 (and never fault with E_WIDTH)") {
    // 59049 nops (cells whose op is Nop at their position); C and D wrap together.
    const auto p = prog(std::string(59049, 'o'));
    ClassicVm vm(p);
    StrIo io;
    vm.set_registers(ClassicRegisters{.a = 0, .c = 59047, .d = 59048});
    CHECK(vm.step(io).status == Status::Continue);
    CHECK(vm.c() == 59048);
    CHECK(vm.d() == 0); // D wrapped, C did not
    CHECK(vm.step(io).status == Status::Continue);
    CHECK(vm.c() == 0); // C wrapped
    CHECK(vm.d() == 1);
}

// A literal, slow rendition of spec 8 and 5 used as the oracle for random programs.
struct Oracle {
    std::vector<std::uint32_t> mem;
    std::uint32_t a = 0, c = 0, d = 0;
    std::string out;
    std::string in;
    std::size_t ip = 0;
    std::uint64_t steps = 0;

    explicit Oracle(const std::vector<std::uint8_t>& p) : mem(p.begin(), p.end()) {
        while (mem.size() < kClassicCells) {
            mem.push_back(classic_op(mem[mem.size() - 1], mem[mem.size() - 2]));
        }
    }
    // 0 continue, 1 halt, 2 E_EXEC, 3 E_ENCRYPT
    int step() {
        ++steps;
        if (mem[c] < 33 || mem[c] > 126) {
            return 2;
        }
        const char op = kXlat1[(c + mem[c] - 33) % 94];
        switch (op) {
        case 'i':
            c = mem[d];
            break;
        case '<':
            out.push_back(static_cast<char>(a % 256));
            break;
        case '/':
            a = ip < in.size() ? static_cast<unsigned char>(in[ip++]) : 59048;
            break;
        case '*':
            a = mem[d] = mem[d] / 3 + mem[d] % 3 * 19683;
            break;
        case 'j':
            d = mem[d];
            break;
        case 'p':
            a = mem[d] = classic_op(a, mem[d]);
            break;
        case 'v':
            return 1;
        default:
            break;
        }
        if (mem[c] < 33 || mem[c] > 126) {
            return 3;
        }
        mem[c] = static_cast<unsigned char>(kXlat2[mem[c] - 33]);
        c = (c + 1) % kClassicCells;
        d = (d + 1) % kClassicCells;
        return 0;
    }
};

TEST_CASE("ClassicVm agrees with a literal oracle on random programs") {
    std::mt19937_64 rng(14);
    for (int iter = 0; iter < 300; ++iter) {
        const auto p = random_program(rng, 2 + rng() % 400);
        std::string input(rng() % 6, '\0');
        for (char& ch : input) {
            ch = static_cast<char>(rng());
        }
        Oracle o(p);
        o.in = input;
        ClassicVm vm(p);
        StrIo io;
        io.in = input;
        int status = 0;
        constexpr std::uint64_t kSteps = 3000;
        while (status == 0 && o.steps < kSteps) {
            status = o.step();
            const StepResult r = vm.step(io);
            const Status expect = status == 0   ? Status::Continue
                                  : status == 1 ? Status::Halted
                                                : Status::Faulted;
            REQUIRE_MESSAGE(r.status == expect, "iter=" << iter << " step=" << o.steps);
            if (r.fault) {
                REQUIRE(r.fault->error == (status == 2 ? Error::Exec : Error::Encrypt));
                REQUIRE(r.fault->steps == o.steps);
                REQUIRE(r.fault->a == nat(o.a));
                REQUIRE(r.fault->c == nat(o.c));
                REQUIRE(r.fault->d == nat(o.d));
                REQUIRE(r.fault->n == 10);
            }
            if (status == 0) {
                const bool same = vm.a() == o.a && vm.c() == o.c && vm.d() == o.d;
                REQUIRE_MESSAGE(same, "iter=" << iter << " step=" << o.steps);
            }
        }
        CHECK(io.out == o.out);
    }
}
