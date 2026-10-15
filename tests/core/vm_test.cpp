#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "core/loader.hpp"
#include "core/tables.hpp"
#include "core/vm.hpp"

namespace {

using namespace malm::core;

// String-backed Io with failure injection.
struct StrIo : Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int flushes = 0;
    bool fail_read = false;
    bool fail_write = false;
    bool fail_flush = false;
    int reads = 0;

    int read_byte() override {
        ++reads;
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
    bool flush() override {
        ++flushes;
        return !fail_flush;
    }
};

std::vector<std::uint8_t> cells(const std::string& text) {
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    auto r = load_text(bytes);
    REQUIRE(r.has_value());
    return *std::move(r);
}

Value v(const char* text) {
    auto r = Value::parse(text);
    REQUIRE(r.has_value());
    return *r;
}

Value nat(std::uint64_t k) { return *Value::natural(k); }

// Cells of a program given as instruction letters (j=MovD i=Jmp *=Rot p=Opr </=Out/In v=Hlt
// o=Nop), assuming every cell is executed for the first time with its own position as C.
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

// Runs to the end and returns the fault (the run must fault).
Fault run_to_fault(Vm& vm, Io& io) {
    const RunResult r = vm.run(io);
    REQUIRE(r.kind == RunResult::Kind::Fault);
    REQUIRE(r.fault.has_value());
    return *r.fault;
}

} // namespace

TEST_CASE("spec 9.1: seven-step trace") {
    const auto prog = cells("u'a%@\"K");
    Vm vm(prog);
    StrIo io;
    io.in = "A";

    struct Row {
        Value a, c, d;
        unsigned n;
        std::vector<std::pair<Value, Value>> cells; // address -> value after the step
    };
    const std::vector<Row> rows = {
        {nat(65), nat(1), nat(1), 10, {{nat(0), nat(111)}}},
        {nat(65), nat(2), nat(40), 10, {{nat(1), nat(116)}}},
        {nat(65), nat(3), nat(41), 10, {{nat(2), nat(46)}}},
        {nat(65), nat(4), v("...1022"), 10, {{nat(3), nat(103)}}},
        {nat(65), nat(5), v("...100"), 10, {{nat(4), nat(81)}}},
        {nat(25), nat(6), v("...101"), 10, {{nat(5), nat(122)}, {v("...100"), nat(25)}}},
    };
    CHECK(vm.n() == 10);
    CHECK(vm.a() == nat(0));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CAPTURE(i + 1);
        const StepResult r = vm.step(io);
        CHECK(r.status == Status::Continue);
        CHECK(vm.steps() == i + 1);
        CHECK(vm.a() == rows[i].a);
        CHECK(vm.c() == rows[i].c);
        CHECK(vm.d() == rows[i].d);
        CHECK(vm.n() == rows[i].n);
        for (const auto& [addr, val] : rows[i].cells) {
            CHECK(vm.memory().read(addr) == val);
        }
    }
    CHECK(io.out == "A");
    const StepResult last = vm.step(io);
    CHECK(last.status == Status::Halted);
    CHECK(vm.steps() == 7);
    CHECK(vm.a() == nat(25));
    CHECK(vm.c() == nat(6));
    CHECK(vm.d() == v("...101"));
    CHECK(vm.n() == 10);
    CHECK(vm.halted());
    CHECK(io.out == "A");
    CHECK(io.flushes >= 2); // before In, and on halt
}

TEST_CASE("spec 9.1 via run()") {
    const auto prog = cells("u'a%@\"K");
    Vm vm(prog);
    StrIo io;
    io.in = "A";
    const RunResult r = vm.run(io);
    CHECK(r.kind == RunResult::Kind::Halted);
    CHECK(io.out == "A");
    CHECK(vm.steps() == 7);
    CHECK(exit_status(r, vm, false) == 0);
    CHECK(exit_status(r, vm, true) == 25);
}

TEST_CASE("spec 9.2: growth event") {
    const auto prog = cells("(&&%M");
    Vm vm(prog);
    StrIo io;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(vm.step(io).status == Status::Continue);
        CHECK(vm.n() == 10);
    }
    REQUIRE(vm.step(io).status == Status::Continue);
    CHECK(vm.c() == nat(4));
    CHECK(vm.d() == v("...12111111221"));
    CHECK(vm.n() == 11);
    CHECK(vm.step(io).status == Status::Halted);
    CHECK(vm.steps() == 5);
}

TEST_CASE("spec 9.4: EOF reads as ...222 and Out writes 168") {
    const auto prog = cells("ubO");
    Vm vm(prog);
    StrIo io;
    REQUIRE(vm.step(io).status == Status::Continue);
    CHECK(vm.a() == Value::from_raw(pow3[40] - 1));
    CHECK(vm.run(io).kind == RunResult::Kind::Halted);
    CHECK(io.out == std::string(1, '\xA8'));
}

TEST_CASE("programs: In/Out pairs copy input, EOF is sticky") {
    Vm vm(prog("/</<v"));
    StrIo io;
    io.in = "xy";
    CHECK(vm.run(io).kind == RunResult::Kind::Halted);
    CHECK(io.out == "xy");

    Vm vm2(prog("///v"));
    StrIo io2;
    io2.in = "q";
    CHECK(vm2.run(io2).kind == RunResult::Kind::Halted);
    CHECK(io2.reads == 2); // 'q', then one EOF read; the third In does not read again
    CHECK(vm2.a() == Value::from_raw(pow3[40] - 1));
}

TEST_CASE("halting step: no encryption, no increment, counted") {
    const auto p = prog("ov");
    Vm vm(p);
    StrIo io;
    const RunResult r = vm.run(io);
    CHECK(r.kind == RunResult::Kind::Halted);
    CHECK(vm.steps() == 2);
    CHECK(vm.c() == nat(1));
    CHECK(vm.d() == nat(1));
    CHECK(vm.memory().read(nat(1)) == nat(p[1])); // not encrypted
    CHECK(vm.memory().read(nat(0)) != nat(p[0])); // the Nop was
    CHECK(io.flushes == 1);
}

TEST_CASE("step after halt stays halted") {
    Vm vm(prog("ov"));
    StrIo io;
    REQUIRE(vm.run(io).kind == RunResult::Kind::Halted);
    CHECK(vm.step(io).status == Status::Halted);
    CHECK(vm.steps() == 2);
}

TEST_CASE("run: step limit") {
    StrIo io;
    {
        Vm vm(prog("ooov"));
        const RunResult r = vm.run(io, 3);
        CHECK(r.kind == RunResult::Kind::StepLimit);
        CHECK(vm.steps() == 3);
        CHECK_FALSE(vm.halted());
        CHECK(vm.run(io, 4).kind == RunResult::Kind::Halted); // 1 more step reaches Hlt
    }
    {
        Vm vm(prog("ooov"));
        CHECK(vm.run(io, 0).kind == RunResult::Kind::StepLimit);
        CHECK(vm.steps() == 0);
    }
    {
        Vm vm(prog("ooov"));
        CHECK(vm.run(io, 4).kind == RunResult::Kind::Halted);
        CHECK(vm.steps() == 4);
    }
}

TEST_CASE("E_EXEC: jump into a fill cell outside 33..126") {
    Vm vm(prog("iov")); // Jmp to [0] (a program cell value ~98), landing in the fill region
    StrIo io;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Exec);
    CHECK(f.steps == 2);
    CHECK(f.c == nat(99));
    CHECK(f.d == nat(1));
    CHECK(f.a == nat(0));
    CHECK(f.n == 10);
    CHECK_FALSE(f.overflow);
    CHECK(io.flushes >= 1);
    CHECK(exit_status(RunResult{RunResult::Kind::Fault, f}, vm, true) == 1);
}

TEST_CASE("E_EXEC: running off the end of the program, registers are from before the step") {
    Vm vm(prog("ooo"));
    StrIo io;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Exec);
    CHECK(f.steps == 4); // the failing step is counted
    CHECK(f.c == nat(3));
    CHECK(f.d == nat(3));
    CHECK(vm.steps() == 4);
}

TEST_CASE("E_ENCRYPT: Opr leaves a non-natural in the cell about to be encrypted") {
    Vm vm(prog("pv")); // A := [0] := crz(0, [0]) has head 1
    StrIo io;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Encrypt);
    CHECK(f.steps == 1);
    CHECK(f.c == nat(0)); // detected before the increment
    CHECK(f.d == nat(0));
    CHECK(f.a.head() == 1);
    CHECK(f.n == 10);
}

TEST_CASE("E_WIDTH: growth rule would exceed 39") {
    Vm vm(prog("ov"));
    // Test hook: D at width 39 (trit 38 set) is one past what n <= 39 can hold after the step.
    vm.set_registers({nat(0), nat(0), nat(pow3[38])});
    StrIo io;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Width);
    CHECK_FALSE(f.overflow);
    CHECK(f.steps == 1);
    CHECK(f.n == 10);     // n before the failed growth
    CHECK(f.c == nat(1)); // registers after step 5
    CHECK(f.d == nat(pow3[38] + 1));
    CHECK(vm.n() == 10);
}

TEST_CASE("E_WIDTH: increment carries into the head trit at n = 39") {
    Vm vm(prog("ooov"));
    StrIo io;
    vm.set_registers({nat(0), nat(0), nat(pow3[37])}); // width 38: step 1 grows n to 39
    REQUIRE(vm.step(io).status == Status::Continue);
    REQUIRE(vm.n() == 39);
    vm.set_registers({nat(0), nat(1), nat(pow3[39] - 1)}); // 39 trits of 2, head 0
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Width);
    CHECK(f.overflow);
    CHECK(f.n == 39);
    CHECK(f.steps == 2);
    CHECK(f.c == nat(1)); // pre-increment registers
    CHECK(f.d == nat(pow3[39] - 1));
}

TEST_CASE("E_IO: write failure") {
    Vm vm(prog("<v"));
    StrIo io;
    io.fail_write = true;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Io);
    CHECK(f.steps == 1);
    CHECK(f.c == nat(0));
    CHECK(io.flushes >= 1);
}

TEST_CASE("E_IO: read error") {
    Vm vm(prog("/v"));
    StrIo io;
    io.fail_read = true;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Io);
    CHECK(f.steps == 1);
    CHECK(f.a == nat(0)); // A not modified
}

TEST_CASE("E_IO: flush failure before In") {
    Vm vm(prog("/v"));
    StrIo io;
    io.in = "x";
    io.fail_flush = true;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Io);
    CHECK(f.steps == 1);
    CHECK(io.reads == 0); // flushed first, and the failure stopped the read
}

TEST_CASE("E_IO: flush failure on halt") {
    Vm vm(prog("ov"));
    StrIo io;
    io.fail_flush = true;
    const Fault f = run_to_fault(vm, io);
    CHECK(f.error == Error::Io);
    CHECK(f.steps == 2);
    CHECK_FALSE(vm.halted());
}

TEST_CASE("exit status") {
    Vm vm(prog("/v"));
    StrIo io;
    io.in = "\x07";
    const RunResult r = vm.run(io);
    REQUIRE(r.kind == RunResult::Kind::Halted);
    CHECK(exit_status(r, vm, false) == 0);
    CHECK(exit_status(r, vm, true) == 7);

    // A mod 256 uses the remainder rule for every head: ...222 is 59048 mod 256 = 168.
    Vm vm2(prog("/v"));
    StrIo io2;
    const RunResult r2 = vm2.run(io2);
    CHECK(exit_status(r2, vm2, true) == 168);
}

TEST_CASE("format_fault") {
    Fault f{Error::Width, nat(5), nat(7), nat(0), 39, 4258, true};
    CHECK(format_fault(f) == "E_WIDTH at step 4258: C=" + nat(5).to_string() +
                                 " D=" + nat(7).to_string() + " A=" + nat(0).to_string() +
                                 " n=39 (overflow)");
    f.overflow = false;
    f.error = Error::Exec;
    CHECK(format_fault(f).starts_with("E_EXEC at step 4258: C="));
    CHECK(format_fault(f).ends_with(" n=39"));
    CHECK(error_name(Error::Encrypt) == "E_ENCRYPT");
    CHECK(error_name(Error::Io) == "E_IO");
}

namespace {

struct CountObs : StepObserver {
    std::uint64_t count = 0;
    std::uint64_t last_step = 0;
    void before(const StepInfo& s) override {
        ++count;
        last_step = s.step;
    }
};

template <class W> W word_at(unsigned i) {
    if constexpr (std::is_same_v<W, Value>) {
        return *Value::natural(i);
    } else {
        return static_cast<W>(i);
    }
}

// run(io, max) and run(io, obs, max) must end in the same state with the same output.
template <class VmT>
void check_same_run(const std::string& text, const std::string& input,
                    std::optional<std::uint64_t> max) {
    StrIo io1;
    StrIo io2;
    io1.in = io2.in = input;
    VmT plain(cells(text));
    VmT observed(cells(text));
    CountObs obs;
    const RunResult r1 = plain.run(io1, max);
    const RunResult r2 = observed.run(io2, obs, max);
    CHECK(r1.kind == r2.kind);
    CHECK(plain.a() == observed.a());
    CHECK(plain.c() == observed.c());
    CHECK(plain.d() == observed.d());
    CHECK(plain.n() == observed.n());
    CHECK(plain.steps() == observed.steps());
    CHECK(plain.halted() == observed.halted());
    CHECK(io1.out == io2.out);
    for (const unsigned i : {0u, 1u, 2u, 5u, 40u, 100u}) {
        CHECK(plain.memory().read(word_at<typename VmT::Word>(i)) ==
              observed.memory().read(word_at<typename VmT::Word>(i)));
    }
    CHECK(obs.count == observed.steps());
    CHECK(obs.last_step == observed.steps());
    if (max) {
        CHECK(observed.steps() <= *max);
    }
}

} // namespace

TEST_CASE("run with an observer reaches the same state as run without, both modes") {
    const std::string hello =
        R"x((=<`#9]~6ZY32Vw/.R,+Op(L,+k#Gh&}Cdz@aw=;zyKw%ut4Uqp0/mlejihtfrHcbaC2^W\>Z,XW)UTSL53\HGFjW)x";
    const std::string cat5 = "ubs`q^o\\mZG";
    for (const std::optional<std::uint64_t> max :
         {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
          std::optional<std::uint64_t>{10}, std::optional<std::uint64_t>{44},
          std::optional<std::uint64_t>{45}, std::optional<std::uint64_t>{1000}}) {
        check_same_run<Vm>(hello, "", max);
        check_same_run<ClassicVm>(hello, "", max);
        check_same_run<Vm>(cat5, "ab", max);
        check_same_run<ClassicVm>(cat5, "ab", max);
        check_same_run<Vm>("DCB", "", max); // E_EXEC at step 4
    }
}

TEST_CASE("observer sees E_EXEC steps undecoded and honours the step limit") {
    struct Rec : StepObserver {
        std::vector<StepInfo> v;
        void before(const StepInfo& s) override { v.push_back(s); }
    };
    StrIo io;
    Vm vm(cells("DCB"));
    Rec rec;
    CHECK(vm.run(io, rec).kind == RunResult::Kind::Fault);
    REQUIRE(rec.v.size() == 4);
    CHECK_FALSE(rec.v[3].decoded);
    CHECK(rec.v[3].step == 4);
    CHECK(rec.v[0].decoded);
    Vm lim(cells("DCB"));
    Rec rec2;
    CHECK(lim.run(io, rec2, 2).kind == RunResult::Kind::StepLimit);
    CHECK(rec2.v.size() == 2);
}

namespace {

// The step order of spec 5 written out plainly: the machine's plain read and write, [C] read again
// for encryption, C mod 94 computed every step. The reference for the step loop (issue #37), which
// keeps C mod 94 up to date incrementally, reads memory through per-register page cursors and
// reads [C] again only when it can have changed.
template <class M> struct RefVm {
    using Word = typename M::Word;
    M m;
    Word a{};
    Word c{};
    Word d{};
    bool eof = false;

    explicit RefVm(std::span<const std::uint8_t> program) : m(program) {}

    // Continue, Halted or Faulted (with `err` set).
    Status step(StrIo& io, Error& err) {
        const Word cell = m.read(c);
        if (!M::is_cell(cell)) {
            err = Error::Exec;
            return Status::Faulted;
        }
        switch (kOpOfCode[(M::mod94(c) + M::cell_code(cell)) % 94]) {
        case Op::Jmp:
            c = m.read(d);
            break;
        case Op::Out:
            io.write_byte(static_cast<std::uint8_t>(M::mod256(a)));
            break;
        case Op::In: {
            const int b = eof ? kEof : io.read_byte();
            eof = b == kEof;
            a = eof ? M::eof() : M::from_byte(static_cast<std::uint8_t>(b));
            break;
        }
        case Op::Rot:
            a = m.rotr(m.read(d));
            m.write(d, a);
            break;
        case Op::MovD:
            d = m.read(d);
            break;
        case Op::Opr:
            a = M::crz(a, m.read(d));
            m.write(d, a);
            break;
        case Op::Nop:
            break;
        case Op::Hlt:
            return Status::Halted;
        }
        const Word target = m.read(c);
        if (!M::is_cell(target)) {
            err = Error::Encrypt;
            return Status::Faulted;
        }
        m.write(c, M::encrypted(target));
        const auto nc = M::incremented(c);
        const auto nd = M::incremented(d);
        if (!nc || !nd || !m.grow(*nc, *nd)) {
            err = Error::Width;
            return Status::Faulted;
        }
        c = *nc;
        d = *nd;
        return Status::Continue;
    }
};

// Runs `program` for up to `max_steps` steps on the VM and on RefVm, from registers (a, c, d) with
// `pokes` written first, and requires the same registers, n and output after every step and the
// same stop.
template <class VmT, class M>
void check_against_reference(
    const std::vector<std::uint8_t>& program, BasicRegisters<typename M::Word> regs,
    const std::vector<std::pair<typename M::Word, typename M::Word>>& pokes,
    const std::string& input, int max_steps) {
    VmT vm(program);
    RefVm<M> ref(program);
    for (const auto& [addr, val] : pokes) {
        vm.poke(addr, val);
        ref.m.write(M::normalize(addr), M::normalize(val));
    }
    vm.set_registers(regs);
    ref.a = vm.a();
    ref.c = vm.c();
    ref.d = vm.d();
    StrIo vio;
    StrIo rio;
    vio.in = rio.in = input;
    for (int i = 0; i < max_steps; ++i) {
        Error err{};
        const Status want = ref.step(rio, err);
        const StepResult got = vm.step(vio);
        REQUIRE(got.status == want);
        if (want == Status::Faulted) {
            REQUIRE(got.fault->error == err);
        }
        REQUIRE(vio.out == rio.out);
        if (want != Status::Continue) {
            return;
        }
        REQUIRE(vm.a() == ref.a);
        REQUIRE(vm.c() == ref.c);
        REQUIRE(vm.d() == ref.d);
        REQUIRE(vm.n() == ref.m.n());
    }
}

std::vector<std::uint8_t> random_cells(std::mt19937_64& rng, std::size_t len) {
    std::vector<std::uint8_t> p(len);
    for (auto& x : p) {
        x = static_cast<std::uint8_t>(33 + rng() % 94);
    }
    return p;
}

} // namespace

TEST_CASE("the step loop matches a plain reference step, both modes") {
    std::mt19937_64 rng(37);
    const std::string input = "Hello\n";
    for (int i = 0; i < 400; ++i) {
        const auto program = random_cells(rng, 2 + rng() % 400);
        check_against_reference<Vm, UnleashedMachine>(program, {}, {}, input, 3000);
        check_against_reference<ClassicVm, ClassicMachine>(program, {}, {}, input, 3000);
    }
}

TEST_CASE("the step loop matches a plain reference step across the ...222 -> 0 wrap") {
    // C (and D) start a few cells below the wrap, on code cells, so the run crosses it: C mod 94
    // jumps from 16 to 0 there instead of counting on.
    std::mt19937_64 rng(3737);
    const std::string input = "xyz";
    for (int i = 0; i < 300; ++i) {
        const auto program = random_cells(rng, 2 + rng() % 200);
        const std::uint64_t below = 1 + rng() % 40;
        const bool d_too = rng() % 2 == 0;

        std::vector<std::pair<Value, Value>> upokes;
        const std::uint64_t top = pow3[kTrits] - 1; // ...222
        for (std::uint64_t k = 0; k <= below; ++k) {
            upokes.emplace_back(Value::from_raw(top - k), nat(33 + rng() % 94));
        }
        const Value uc = Value::from_raw(top - below);
        const Value ud = d_too ? Value::from_raw(top - rng() % 40) : nat(rng() % 200);
        check_against_reference<Vm, UnleashedMachine>(program, {nat(rng() % 300), uc, ud}, upokes,
                                                      input, 2000);

        std::vector<std::pair<ClassicWord, ClassicWord>> cpokes;
        for (std::uint64_t k = 0; k <= below; ++k) {
            cpokes.emplace_back(static_cast<ClassicWord>(kClassicEof - k),
                                static_cast<ClassicWord>(33 + rng() % 94));
        }
        const auto cc = static_cast<ClassicWord>(kClassicEof - below);
        const auto cd = d_too ? static_cast<ClassicWord>(kClassicEof - rng() % 40)
                              : static_cast<ClassicWord>(rng() % 200);
        check_against_reference<ClassicVm, ClassicMachine>(
            program, {static_cast<ClassicWord>(rng() % 300), cc, cd}, cpokes, input, 2000);
    }
}
