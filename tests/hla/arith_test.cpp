// The arithmetic statements (issue #21) in the VM against C++ reference functions.
//
// One harness program holds every statement form as a case of a switch:
//     loop: in t, done / switch t, f0, f1, ... / f<i>: <statement> / jmp loop
// The test's I/O hook runs between two cases: it reads the registers back and checks them against
// the model, then pokes the next operands into the registers and returns the next case's number.
// The steps between two reads, minus those of an empty case, are the statement's cost.
//
// Fast: every form on edge values and random words. Slow: exhaustive on 8-bit operands and 10 000
// random cases per operation, on several threads (each its own VM).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hla_test_util.hpp"

using namespace hlatest;

namespace {

struct Regs {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
};

std::int32_t s32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

// The C++ reference of an operation (RV32I semantics).
std::uint32_t ref(const std::string& op, std::uint32_t a, std::uint32_t b) {
    if (op == "add") {
        return a + b;
    }
    if (op == "sub") {
        return a - b;
    }
    if (op == "and") {
        return a & b;
    }
    if (op == "or") {
        return a | b;
    }
    if (op == "xor") {
        return a ^ b;
    }
    if (op == "slt") {
        return s32(a) < s32(b) ? 1 : 0;
    }
    if (op == "sltu") {
        return a < b ? 1 : 0;
    }
    if (op == "sll") {
        return a << (b & 31);
    }
    if (op == "srl") {
        return a >> (b & 31);
    }
    if (op == "sra") {
        return static_cast<std::uint32_t>(s32(a) >> (b & 31));
    }
    if (op == "sextb") {
        return static_cast<std::uint32_t>(static_cast<std::int8_t>(a));
    }
    if (op == "sexth") {
        return static_cast<std::uint32_t>(static_cast<std::int16_t>(a));
    }
    return 0xDEADBEEF; // not an operation of make_forms()
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

// One statement form: its text and its effect on the registers.
struct Form {
    std::string op;
    std::string text;
    std::function<void(Regs&)> model;
};

const std::vector<std::string> kBinary = {"add", "sub",  "and", "or",  "xor",
                                          "slt", "sltu", "sll", "srl", "sra"};

bool is_shift_op(const std::string& op) { return op == "sll" || op == "srl" || op == "sra"; }

std::vector<std::uint32_t> constants(const std::string& op) {
    if (is_shift_op(op)) {
        return {0, 1, 4, 8, 13, 31};
    }
    if (op == "and" || op == "or" || op == "xor") {
        return {0, 0xFF, 0xFFFF0000, 0xFFFFFFFF, 0x5A5A5A5A};
    }
    return {0, 1, 0xFFFFFFFF, 0x80000000, 0x7FF, 0xFFFFF800, 0x12345678};
}

std::vector<Form> make_forms() {
    std::vector<Form> f;
    for (const std::string& op : kBinary) {
        f.push_back({op, op + " z, x, y", [op](Regs& r) { r.z = ref(op, r.x, r.y); }});
        f.push_back({op, op + " x, x, y", [op](Regs& r) { r.x = ref(op, r.x, r.y); }});
        f.push_back({op, op + " y, x, y", [op](Regs& r) { r.y = ref(op, r.x, r.y); }});
        f.push_back({op, op + " z, x, x", [op](Regs& r) { r.z = ref(op, r.x, r.x); }});
        for (const std::uint32_t k : constants(op)) {
            f.push_back({op, op + " z, x, " + (is_shift_op(op) ? std::to_string(k) : hex(k)),
                         [op, k](Regs& r) { r.z = ref(op, r.x, k); }});
        }
        const std::uint32_t k = is_shift_op(op) ? 5 : 0x0F0F00F1;
        f.push_back(
            {op, op + " x, x, " + std::to_string(k), [op, k](Regs& r) { r.x = ref(op, r.x, k); }});
    }
    for (const std::string op : {"sextb", "sexth"}) {
        f.push_back({op, op + " z, x", [op](Regs& r) { r.z = ref(op, r.x, 0); }});
        f.push_back({op, op + " x, x", [op](Regs& r) { r.x = ref(op, r.x, 0); }});
    }
    return f;
}

const std::vector<Form>& forms() {
    static const std::vector<Form> f = make_forms();
    return f;
}

// A harness program: case i runs forms()[forms[i]], the last case (forms.size()) is empty.
struct Harness {
    std::vector<std::size_t> forms;
    std::string source;
    Compiled compiled;
};

Harness make_harness(std::vector<std::size_t> fs) {
    std::string s = "reg x, y, z, t\n"
                    "loop:   in t, done\n"
                    "        switch t";
    for (std::size_t i = 0; i <= fs.size(); ++i) {
        s += ", f" + std::to_string(i);
    }
    s += "\n        halt 1\n";
    for (std::size_t i = 0; i < fs.size(); ++i) {
        s += "f" + std::to_string(i) + ":     " + forms()[fs[i]].text + "\n        jmp loop\n";
    }
    s += "f" + std::to_string(fs.size()) + ":     jmp loop\n";
    s += "done:   halt\n";
    Compiled c = compile(s);
    return {std::move(fs), std::move(s), std::move(c)};
}

// A debug build has sanitizers, and its assembler and VM are ~70x slower.
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// Every form.
const Harness& full_harness() {
    static const Harness h = [] {
        std::vector<std::size_t> fs(forms().size());
        for (std::size_t i = 0; i < fs.size(); ++i) {
            fs[i] = i;
        }
        return make_harness(std::move(fs));
    }();
    return h;
}

// Per operation the register form and one with a constant. A debug build runs the fast case on
// these only; the slow cases use every form.
const Harness& core_harness() {
    static const Harness h = [] {
        std::vector<std::size_t> fs;
        std::map<std::string, int> seen;
        for (std::size_t i = 0; i < forms().size(); ++i) {
            const Form& f = forms()[i];
            const bool reg_form = f.text == f.op + " z, x, y" || f.text == f.op + " z, x";
            const bool const_form =
                f.text.starts_with(f.op + " z, x, ") && !reg_form && f.text != f.op + " z, x, x";
            if (reg_form || (const_form && seen[f.op]++ == 1)) {
                fs.push_back(i);
            }
        }
        return make_harness(std::move(fs));
    }();
    return h;
}

struct Case {
    std::size_t form;
    Regs in;
};

struct Failure {
    std::size_t form;
    Regs in;
    Regs want;
    Regs got;
};

// The I/O hook: checks the case that just ran, starts the next one.
struct Hook final : core::Io {
    core::Vm* vm = nullptr;
    const Harness* harness = nullptr;
    const std::vector<Case>* cases = nullptr;
    std::size_t next = 0;
    core::Value ax, ay, az;
    std::vector<Failure> failures;
    std::map<std::size_t, std::pair<std::uint64_t, std::uint64_t>> steps; // form -> sum, count
    std::uint64_t started = 0;

    std::uint32_t get(core::Value at) const {
        const auto w = hla::word_value(vm->memory().read(at));
        return w ? *w : 0xDEADBEEF;
    }

    int read_byte() override {
        if (next > 0) {
            const Case& c = (*cases)[next - 1];
            Regs want = c.in;
            if (c.form < forms().size()) {
                forms()[c.form].model(want);
            }
            const Regs got{get(ax), get(ay), get(az)};
            if (got.x != want.x || got.y != want.y || got.z != want.z) {
                failures.push_back({c.form, c.in, want, got});
            }
            auto& [sum, n] = steps[c.form];
            sum += vm->steps() - started;
            ++n;
        }
        if (next == cases->size()) {
            return core::kEof;
        }
        const Case& c = (*cases)[next++];
        vm->poke(ax, hla::word(c.in.x));
        vm->poke(ay, hla::word(c.in.y));
        vm->poke(az, hla::word(c.in.z));
        started = vm->steps();
        const auto& fs = harness->forms;
        return static_cast<int>(
            c.form == forms().size()
                ? fs.size()
                : static_cast<std::size_t>(std::find(fs.begin(), fs.end(), c.form) - fs.begin()));
    }
    bool write_byte(std::uint8_t) override { return true; }
    bool flush() override { return true; }
};

core::Value label(const Compiled& c, const std::string& name) {
    return c.assembly.layout.labels.at(name).addr;
}

// Runs the cases in one VM; returns the hook with the failures and the step counts.
Hook run_cases(const Harness& hs, const std::vector<Case>& cases) {
    const Compiled& c = hs.compiled;
    core::Vm vm(c.assembly.startup.text);
    Hook h;
    h.vm = &vm;
    h.harness = &hs;
    h.cases = &cases;
    h.ax = label(c, "V_x");
    h.ay = label(c, "V_y");
    h.az = label(c, "V_z");
    const auto res = vm.run(h);
    if (res.fault) {
        h.failures.push_back({0, {}, {}, {}});
    }
    if (h.next != cases.size() || res.kind != core::RunResult::Kind::Halted) {
        h.failures.push_back({0, {}, {}, {0xBAD, 0xBAD, 0xBAD}});
    }
    return h;
}

// The same, split over threads; the failures of all of them.
std::vector<Failure> run_parallel(const std::vector<Case>& cases) {
    const Harness& hs = full_harness(); // compiled once, before the threads
    const unsigned n = std::clamp(std::thread::hardware_concurrency(), 1U, 8U);
    std::vector<std::vector<Case>> parts(n);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        parts[i % n].push_back(cases[i]);
    }
    std::vector<Failure> failures;
    std::mutex mu;
    std::vector<std::thread> threads;
    for (const auto& part : parts) {
        threads.emplace_back([&, &part = part] {
            const Hook h = run_cases(hs, part);
            const std::lock_guard lock(mu);
            failures.insert(failures.end(), h.failures.begin(), h.failures.end());
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    return failures;
}

void report(const std::vector<Failure>& failures) {
    for (std::size_t i = 0; i < std::min<std::size_t>(failures.size(), 20); ++i) {
        const Failure& f = failures[i];
        INFO(forms()[f.form].text << " with x=" << hex(f.in.x) << " y=" << hex(f.in.y)
                                  << " z=" << hex(f.in.z) << ": want x=" << hex(f.want.x)
                                  << " y=" << hex(f.want.y) << " z=" << hex(f.want.z)
                                  << ", got x=" << hex(f.got.x) << " y=" << hex(f.got.y)
                                  << " z=" << hex(f.got.z));
        CHECK(false);
    }
    CHECK(failures.empty());
}

std::uint32_t sext8(std::uint32_t b) {
    return static_cast<std::uint32_t>(static_cast<std::int8_t>(b));
}

const std::vector<std::uint32_t> kEdges = {0,          1,          2,          0x7F,
                                           0x80,       0xFF,       0x7FFFFFFF, 0x80000000,
                                           0xFFFFFFFF, 0xFFFFFFFE, 0x0000FFFF, 0x12345678};

} // namespace

TEST_CASE("arithmetic statements match the C++ reference, every form, and their cost") {
    const Harness& hs = kDebugBuild ? core_harness() : full_harness();
    std::mt19937 rng(21);
    std::vector<Case> cases;
    for (const std::size_t f : hs.forms) {
        for (int i = 0; i < 24; ++i) {
            const auto pick = [&] {
                return i < 12 ? kEdges[rng() % kEdges.size()] : static_cast<std::uint32_t>(rng());
            };
            cases.push_back({f, {pick(), pick(), static_cast<std::uint32_t>(rng())}});
        }
    }
    for (int i = 0; i < 24; ++i) {
        cases.push_back({forms().size(), {}}); // the empty case: the harness's own steps
    }
    std::shuffle(cases.begin(), cases.end(), rng);
    // Variable shift amounts: also bytes above 31 (the switch looks at the low byte).
    for (Case& c : cases) {
        if (c.form < forms().size() && is_shift_op(forms()[c.form].op) && rng() % 2) {
            c.in.y = static_cast<std::uint32_t>(rng());
        }
    }
    INFO(hs.source);
    const Hook h = run_cases(hs, cases);
    report(h.failures);

    // Steps per statement (docs/hla.md 4.7 records them): mean over the cases, minus the empty
    // case.
    const auto mean = [&](std::size_t form) {
        const auto& [sum, n] = h.steps.at(form);
        return static_cast<double>(sum) / static_cast<double>(n);
    };
    const double base = mean(forms().size());
    std::string table;
    for (const std::size_t f : hs.forms) {
        char line[96];
        std::snprintf(line, sizeof line, "%-24s %6.0f\n", forms()[f].text.c_str(), mean(f) - base);
        table += line;
        CHECK(mean(f) - base < 6000); // a regression guard far above the baseline
    }
    MESSAGE("steps per statement in the harness (empty case " << base << " steps):\n" << table);
}

TEST_CASE("arithmetic: exhaustive on 8-bit operands" * doctest::test_suite("slow")) {
    // x and y sign-extended 8-bit values (every lane of the word sees both signs); shift amounts
    // the 256 bytes (the low 5 bits count). A debug build runs every 16th pair.
    const std::uint32_t stride = kDebugBuild ? 16 : 1;
    std::vector<Case> cases;
    for (std::size_t f = 0; f < forms().size(); ++f) {
        const std::string& text = forms()[f].text;
        if (text != forms()[f].op + " z, x, y" && text != forms()[f].op + " z, x") {
            continue;
        }
        const bool shift = is_shift_op(forms()[f].op);
        for (std::uint32_t p = 0; p < 65536; p += stride) {
            const std::uint32_t i = p >> 8;
            const std::uint32_t j = p & 255;
            if (forms()[f].op.starts_with("sext") && j != 0) {
                continue; // one operand: x over its 256 values
            }
            cases.push_back({f, {sext8(i), shift ? j : sext8(j), 0x5A5A5A5A}});
        }
    }
    MESSAGE(cases.size() << " cases");
    report(run_parallel(cases));
}

TEST_CASE("arithmetic: 10 000 random 32-bit cases per operation" * doctest::test_suite("slow")) {
    std::mt19937 rng(2121);
    std::vector<Case> cases;
    std::map<std::string, int> per_op;
    for (std::size_t f = 0; f < forms().size(); ++f) {
        const std::string& text = forms()[f].text;
        if (text == forms()[f].op + " z, x, y" || text == forms()[f].op + " z, x") {
            for (int i = 0; i < 10000; ++i) {
                cases.push_back(
                    {f,
                     {static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(rng()),
                      static_cast<std::uint32_t>(rng())}});
            }
            per_op[forms()[f].op] += 10000;
        }
    }
    CHECK(per_op.size() == kBinary.size() + 2);
    report(run_parallel(cases));
}
