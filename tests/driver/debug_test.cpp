// `malm run --trace` and `--stats` (docs/cli.md). The golden traces tests/programs/hello.*.trace
// come from the executable spec model, not from this VM (see tests/programs/SOURCES.txt).
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "driver/cli.hpp"

namespace {

struct Result {
    int code;
    std::string out;
    std::string err;
};

Result run(std::vector<std::string_view> args, const std::string& input = "") {
    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream err;
    const int code = malm::run_cli(args, in, out, err);
    return {code, out.str(), err.str()};
}

[[maybe_unused]] std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>(f), {}};
}

[[maybe_unused]] std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    for (std::string l; std::getline(is, l);) {
        v.push_back(l);
    }
    return v;
}

[[maybe_unused]] std::string join(const std::vector<std::string>& v, std::size_t from,
                                  std::size_t to) {
    std::string s;
    for (std::size_t i = from; i < to && i < v.size(); ++i) {
        s += v[i] + '\n';
    }
    return s;
}

const std::string kProg = MALM_PROGRAMS_DIR;
[[maybe_unused]] const std::string kCorpus = MALM_CORPUS_DIR;

// The stderr up to (excluding) the "wall" row: everything deterministic of a --stats block.
std::string stats_block(const std::string& err) {
    const std::size_t at = err.find("malm stats:\n");
    REQUIRE(at != std::string::npos);
    const std::size_t wall = err.find("  wall ", at);
    REQUIRE(wall != std::string::npos);
    return err.substr(at, wall - at);
}

std::string block(std::string_view steps, std::string_view halted, std::string_view peak,
                  std::string_view pages, const std::vector<std::uint64_t>& ops) {
    static constexpr const char* names[8] = {"Jmp",  "Out", "In",  "Rot",
                                             "MovD", "Opr", "Nop", "Hlt"};
    auto row = [](std::string label, const std::string& v) {
        label = "  " + label;
        label.resize(15, ' ');
        return label + v + '\n';
    };
    std::string s = "malm stats:\n";
    s += row("steps", std::string(steps)) + row("halted", std::string(halted)) +
         row("n", std::string(peak)) + row("pages", std::string(pages));
    for (std::size_t i = 0; i < 8; ++i) {
        s += row(std::string("op ") + names[i], std::to_string(ops[i]));
    }
    return s;
}

// Per-op counts of a golden trace file (an independent source of the expected statistics).
[[maybe_unused]] std::vector<std::uint64_t> counts_of(const std::vector<std::string>& trace) {
    static constexpr const char* names[8] = {"Jmp",  "Out", "In",  "Rot",
                                             "MovD", "Opr", "Nop", "Hlt"};
    std::vector<std::uint64_t> c(8, 0);
    for (const std::string& l : trace) {
        for (std::size_t i = 0; i < 8; ++i) {
            if (l.find(std::string(" op=") + names[i] + ' ') != std::string::npos) {
                ++c[i];
            }
        }
    }
    return c;
}

} // namespace

// 13.8M steps: slow under ASan, so it runs in the slow suite.
TEST_CASE("stats: classic beer99 (op counts from the spec model, sum to the step count)" *
          doctest::test_suite("slow")) {
    const Result r = run({"run", "--classic", "--stats", kProg + "/beer99.mal"});
    CHECK(r.code == 0);
    CHECK(r.out.size() == 11459);
    const std::vector<std::uint64_t> ops = {7330597, 11459, 0, 408530, 1777986, 333179, 3940854, 1};
    std::uint64_t sum = 0;
    for (const std::uint64_t o : ops) {
        sum += o;
    }
    CHECK(sum == 13802606);
    CHECK(stats_block(r.err) == block("13802606", "yes", "10", "-", ops));
}

TEST_CASE("trace: classic hello matches the golden trace from the spec model") {
    const std::string golden = read_all(kProg + "/hello.classic.trace");
    REQUIRE(lines(golden).size() == 45);
    const Result r = run({"run", "--classic", "--trace", kProg + "/hello.mal"});
    CHECK(r.code == 0);
    CHECK(r.out == "Hello, world.");
    CHECK(r.err == golden);
}

TEST_CASE("trace: unleashed hello matches the golden trace from the spec model") {
    const std::string golden = read_all(kProg + "/hello.unleashed.trace");
    REQUIRE(lines(golden).size() == 45);
    const Result r = run({"run", "--trace", kProg + "/hello.mal"});
    CHECK(r.code == 0);
    CHECK(r.out == "Hello, world.");
    CHECK(r.err == golden);
}

TEST_CASE("trace: windows are inclusive slices of the full trace") {
    const std::vector<std::string> full = lines(read_all(kProg + "/hello.classic.trace"));
    struct Case {
        std::vector<std::string_view> opts;
        std::size_t from, to; // 0-based slice [from, to)
    };
    const std::vector<Case> cases = {
        {{"--trace-from", "10", "--trace-to", "20"}, 9, 20},
        {{"--trace-to", "20", "--trace-from", "10"}, 9, 20},
        {{"--trace-from", "40"}, 39, 45},
        {{"--trace-to", "3"}, 0, 3},
        {{"--trace-from", "7", "--trace-to", "7"}, 6, 7},
        {{"--trace-from", "1", "--trace-to", "45"}, 0, 45},
        {{"--trace-from", "46"}, 45, 45},
        {{"--trace-from", "44", "--trace-to", "1000"}, 43, 45},
    };
    for (const Case& c : cases) {
        std::vector<std::string_view> args = {"run", "--classic", "--trace"};
        args.insert(args.end(), c.opts.begin(), c.opts.end());
        const std::string hello = kProg + "/hello.mal";
        args.push_back(hello);
        const Result r = run(args);
        CHECK(r.code == 0);
        CHECK(r.err == join(full, c.from, c.to));
    }
}

TEST_CASE("trace: invalid windows are usage errors") {
    const std::string hello = kProg + "/hello.mal";
    const std::vector<std::vector<std::string_view>> bad = {
        {"run", "--trace", "--trace-from", "0", hello},
        {"run", "--trace", "--trace-to", "0", hello},
        {"run", "--trace", "--trace-from", "abc", hello},
        {"run", "--trace", "--trace-from", "-1", hello},
        {"run", "--trace", "--trace-from", "+1", hello},
        {"run", "--trace", "--trace-from", "1x", hello},
        {"run", "--trace", "--trace-from", "", hello},
        {"run", "--trace", "--trace-from", "18446744073709551616", hello},
        {"run", "--trace", "--trace-from", hello},
        {"run", "--trace", hello, "--trace-to"},
        {"run", "--trace", "--trace-from", "5", "--trace-to", "4", hello},
        {"run", "--trace-from", "1", hello},
        {"run", "--trace-to", "5", hello},
    };
    for (const auto& args : bad) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm run") != std::string::npos);
        CHECK(r.err.find("step=") == std::string::npos);
    }
    // from == to and the largest representable step are fine.
    CHECK(run({"run", "--trace", "--trace-from", "18446744073709551615", hello}).code == 0);
}

TEST_CASE("stats: hello, classic and unleashed") {
    const std::vector<std::string> golden = lines(read_all(kProg + "/hello.classic.trace"));
    const std::vector<std::uint64_t> ops = counts_of(golden);
    CHECK(counts_of(lines(read_all(kProg + "/hello.unleashed.trace"))) == ops);
    for (const bool classic : {true, false}) {
        std::vector<std::string_view> args = {"run", "--stats"};
        if (classic) {
            args.push_back("--classic");
        }
        const std::string hello = kProg + "/hello.mal";
        args.push_back(hello);
        const Result r = run(args);
        CHECK(r.code == 0);
        CHECK(r.out == "Hello, world.");
        CHECK(stats_block(r.err) == block("45", "yes", "10", classic ? "-" : "1", ops));
        // The two timing rows come last.
        const std::vector<std::string> l = lines(r.err);
        REQUIRE(l.size() == 15);
        CHECK(l[13].starts_with("  wall         "));
        CHECK(l[13].ends_with(" s"));
        CHECK(l[14].starts_with("  steps/s      "));
    }
}

TEST_CASE("trace: unleashed beer99 matches the golden trace through the E_WIDTH step") {
    const std::string golden = read_all(kProg + "/beer99.unleashed.trace");
    REQUIRE(lines(golden).size() == 4258);
    const Result r = run({"run", "--trace", kProg + "/beer99.mal"});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with(golden));
    CHECK(r.err.substr(golden.size()).starts_with("malm: E_WIDTH at step 4258:"));
}

TEST_CASE("stats: unleashed beer99 ends with E_WIDTH, n 39") {
    // Op counts from the spec model; it counts the faulting step's op (MovD) as decoded.
    const Result r = run({"run", "--stats", kProg + "/beer99.mal"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_WIDTH at step 4258:"));
    CHECK(stats_block(r.err) ==
          block("4258", "no (E_WIDTH)", "39", "107", {1, 0, 0, 793, 1356, 568, 1540, 0}));
}

TEST_CASE("faults: the faulting step's trace line precedes the diagnostic, then the stats") {
    {
        // E_EXEC at the fetch check: the op is not decoded (op=?) and is not counted.
        const Result r = run({"run", "--trace", "--stats", kCorpus + "/exec_fault.mal"});
        CHECK(r.code == 1);
        const std::string expect =
            "step=1 C=0t0 [C]=117 op=In D=0t0 [D]=0t11100 A=0t0 n=10\n"
            "step=2 C=0t1 [C]=98 op=Out D=0t1 [D]=0t10122 A=...2 n=10\n"
            "step=3 C=0t2 [C]=66 op=Nop D=0t2 [D]=0t2110 A=...2 n=10\n"
            "step=4 C=0t10 [C]=...10022 op=? D=0t10 [D]=...10022 A=...2 n=10\n"
            "malm: E_EXEC at step 4: C=0t10 D=0t10 A=...2 n=10\n";
        CHECK(r.err.starts_with(expect));
        CHECK(stats_block(r.err.substr(expect.size())) ==
              block("4", "no (E_EXEC)", "10", "1", {0, 1, 1, 0, 0, 0, 1, 0}));
    }
    {
        // E_ENCRYPT happens after decode: the step's op (Opr) is counted.
        const Result r = run({"run", "--trace", "--stats", kCorpus + "/encrypt_fault.mal"});
        CHECK(r.code == 1);
        const std::string expect = "step=1 C=0t0 [C]=117 op=In D=0t0 [D]=0t11100 A=0t0 n=10\n"
                                   "step=2 C=0t1 [C]=98 op=Out D=0t1 [D]=0t10122 A=...2 n=10\n"
                                   "step=3 C=0t2 [C]=60 op=Opr D=0t2 [D]=0t2020 A=...2 n=10\n"
                                   "malm: E_ENCRYPT at step 3: C=0t2 D=0t2 A=0t1010 n=10\n";
        CHECK(r.err.starts_with(expect));
        CHECK(stats_block(r.err.substr(expect.size())) ==
              block("3", "no (E_ENCRYPT)", "10", "1", {0, 1, 1, 0, 0, 1, 0, 0}));
    }
    {
        // A fault without --trace still prints the diagnostic before the stats.
        const Result r = run({"run", "--stats", kCorpus + "/exec_fault.mal"});
        CHECK(r.err.starts_with("malm: E_EXEC at step 4:"));
        CHECK(r.err.find("step=") == std::string::npos);
    }
    {
        // A window ending before the fault leaves only the window.
        const Result r = run({"run", "--trace", "--trace-to", "2", kCorpus + "/exec_fault.mal"});
        const std::vector<std::string> l = lines(r.err);
        REQUIRE(l.size() == 3);
        CHECK(l[1].starts_with("step=2 "));
        CHECK(l[2].starts_with("malm: E_EXEC"));
    }
}

TEST_CASE("trace and stats leave program output and exit status alone") {
    const std::string cat5 = kProg + "/cat5.mal";
    const std::string in = read_all(kProg + "/cat5_input.bin");
    const std::string expected = read_all(kProg + "/cat5_expected.bin");
    for (const bool classic : {false, true}) {
        std::vector<std::string_view> base = {"run", "--exit-a"};
        if (classic) {
            base.push_back("--classic");
        }
        std::vector<std::string_view> dbg = base;
        dbg.insert(dbg.end(), {"--trace", "--stats"});
        base.push_back(cat5);
        dbg.push_back(cat5);
        const Result plain = run(base, in);
        const Result both = run(dbg, in);
        CHECK(plain.out == expected);
        CHECK(both.out == expected);
        CHECK(both.code == plain.code);
        CHECK(plain.err.empty());
        CHECK(both.err.find("malm stats:") != std::string::npos);
    }
    // --stats alone prints nothing but the block.
    const Result stats = run({"run", "--stats", kProg + "/hello.mal"});
    CHECK(stats.err.starts_with("malm stats:\n"));
    CHECK(stats.err.find("step=") == std::string::npos);
    // A program that does not load prints no stats block.
    const Result missing = run({"run", "--stats", "/nonexistent/x.mal"});
    CHECK(missing.code == 1);
    CHECK(missing.err.find("malm stats:") == std::string::npos);
}

TEST_CASE("trace: a halting-cat reads input and shows A") {
    // cat5: (In Out) x5 Hlt; the trace shows A after each In at the next step.
    const Result r = run({"run", "--trace", kProg + "/cat5.mal"}, "ab");
    const std::vector<std::string> l = lines(r.err);
    REQUIRE(l.size() >= 3);
    CHECK(l[0].find(" op=In ") != std::string::npos);
    CHECK(l[1].find(" op=Out ") != std::string::npos);
    CHECK(l[1].find(" A=0t10121 ") != std::string::npos); // 'a' = 97
}
