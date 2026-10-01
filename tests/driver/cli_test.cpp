#include <doctest/doctest.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

constexpr std::array<std::string_view, 7> kSubcommands{"run",   "asm",  "hla",   "disasm",
                                                       "build", "pack", "export"};

} // namespace

TEST_CASE("help lists all subcommands") {
    for (std::string_view flag : {"--help", "-h", "help"}) {
        const Result r = run({flag});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        for (std::string_view sub : kSubcommands) {
            CHECK_MESSAGE(
                r.out.find(std::string("  ") + std::string(sub) + ' ') != std::string::npos, sub);
        }
    }
}

TEST_CASE("version") {
    const Result r = run({"--version"});
    CHECK(r.code == 0);
    CHECK(r.out.starts_with("malm "));
    CHECK(r.out.ends_with("\n"));
}

TEST_CASE("no args prints usage to stderr and exits 2") {
    const Result r = run({});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.find("usage:") != std::string::npos);
}

TEST_CASE("unknown command") {
    const Result r = run({"frobnicate"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.find("malm: unknown command 'frobnicate'") != std::string::npos);
}

TEST_CASE("known subcommands are not implemented yet") {
    for (std::string_view sub : kSubcommands) {
        if (sub == "run") {
            continue;
        }
        const Result r = run({sub, "x"});
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == "malm " + std::string(sub) + ": not implemented\n");
    }
}

namespace {

// Writes `content` to a fresh temp file and removes it on destruction.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& content) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("malm_cli_test_" + std::to_string(static_cast<long>(getpid())) + "_" +
                std::to_string(counter++) + ".mal");
        std::ofstream(path, std::ios::binary) << content;
    }
    ~TempFile() { std::filesystem::remove(path); }
    std::string str() const { return path.string(); }
};

} // namespace

TEST_CASE("run: usage errors exit 2") {
    for (const std::vector<std::string_view>& args :
         std::vector<std::vector<std::string_view>>{{"run"},
                                                    {"run", "--exit-a"},
                                                    {"run", "--bogus", "f"},
                                                    {"run", "--classic", "f"},
                                                    {"run", "--trace", "f"},
                                                    {"run", "--stats", "f"},
                                                    {"run", "a", "b"}}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm run <file> [--exit-a]") != std::string::npos);
    }
}

TEST_CASE("run: missing file exits 1") {
    const Result r = run({"run", "/nonexistent/malm_no_such_file.mal"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: "));
    CHECK(r.err.find("malm_no_such_file.mal") != std::string::npos);
}

TEST_CASE("run: E_LOAD message") {
    const TempFile f("(A");
    const Result r = run({"run", f.str()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_LOAD at byte 1: "));
}

TEST_CASE("run: E_LOAD for a too-short program reports the input length") {
    const TempFile f("( \n");
    const Result r = run({"run", f.str()});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_LOAD at byte 3: "));
}

TEST_CASE("run: halting program") {
    const TempFile f("(P"); // jv: MovD, Hlt
    const Result r = run({"run", f.str()});
    CHECK(r.code == 0);
    CHECK(r.out.empty());
    CHECK(r.err.empty());
}

TEST_CASE("run: spec 9.1 reads stdin and writes stdout; --exit-a exits with A mod 256") {
    const TempFile f("u'a%@\"K");
    Result r = run({"run", f.str()}, "A");
    CHECK(r.code == 0);
    CHECK(r.out == "A");
    CHECK(r.err.empty());

    r = run({"run", f.str(), "--exit-a"}, "A");
    CHECK(r.code == 25);
    CHECK(r.out == "A");

    r = run({"run", "--exit-a", f.str()}, "A");
    CHECK(r.code == 25);
}

TEST_CASE("run: runtime fault prints diagnostic and exits 1") {
    const TempFile f("DCB"); // ooo: runs off the end -> E_EXEC at step 4
    const Result r = run({"run", f.str(), "--exit-a"});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_EXEC at step 4: C="));
}

TEST_CASE("run: binary-safe I/O") {
    // Straight-line cat of exactly 5 bytes: (In Out) x5, Hlt.
    const TempFile f("ubs`q^o\\mZG");
    const std::string data("a\0\xff\n\xa8", 5);
    const Result r = run({"run", f.str()}, data);
    CHECK(r.code == 0);
    CHECK(r.out == data);
    CHECK(r.err.empty());
}

TEST_CASE("run: committed programs") {
    const std::string dir = MALM_PROGRAMS_DIR;

    // Hand-written 2-cell halting program.
    Result r = run({"run", dir + "/halt2.mal"});
    CHECK(r.code == 0);
    CHECK(r.out.empty());

    // Classic hello world, valid in unleashed mode.
    r = run({"run", dir + "/hello.mal"});
    CHECK(r.code == 0);
    CHECK(r.out == "Hello, world.");
    CHECK(r.err.empty());
}

TEST_CASE("run: a directory is rejected, not aborted on") {
    const Result r = run({"run", std::filesystem::temp_directory_path().string()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: cannot read '"));
    CHECK(r.err.find("not a regular file") != std::string::npos);
}

TEST_CASE("run: -- ends option parsing") {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("-malm_dashfile_" + std::to_string(static_cast<long>(getpid())));
    std::ofstream(p, std::ios::binary) << "(P";
    const Result r = run({"run", "--", p.string()});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    std::filesystem::remove(p);
    // Without it a dash-leading name is an unknown option.
    CHECK(run({"run", "-x.mal"}).code == 2);
}

TEST_CASE("run: E_ENCRYPT diagnostic and exit 1") {
    const TempFile f(">P"); // pv: Opr leaves a non-natural in the cell to encrypt
    const Result r = run({"run", f.str()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_ENCRYPT at step 1: C="));
}

TEST_CASE("run: E_WIDTH from a real program (n ratchets to 39), diagnostic and exit 1") {
    const Result r = run({"run", std::string(MALM_PROGRAMS_DIR) + "/beer99_width.mal"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_WIDTH at step 4258: C="));
    CHECK(r.err.find(" n=39") != std::string::npos);
    CHECK(r.err.find("(overflow)") == std::string::npos);
}
