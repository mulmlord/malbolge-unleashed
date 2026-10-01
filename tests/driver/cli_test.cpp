#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "core/classic.hpp"
#include "core/tables.hpp"
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
        if (sub == "run" || sub == "export") {
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

// Enters a fresh temp directory holding the dash-leading file "-malm_dash" (contents "(P"), so
// the relative name really starts with '-'. Restores the working directory and cleans up.
struct DashDir {
    std::filesystem::path old = std::filesystem::current_path();
    std::filesystem::path dir;
    DashDir() {
        dir = std::filesystem::temp_directory_path() /
              ("malm_dashdir_" + std::to_string(static_cast<long>(getpid())));
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
        std::ofstream("-malm_dash", std::ios::binary) << "(P";
    }
    ~DashDir() {
        std::filesystem::current_path(old);
        std::filesystem::remove_all(dir);
    }
};

} // namespace

TEST_CASE("run: usage errors exit 2") {
    for (const std::vector<std::string_view>& args :
         std::vector<std::vector<std::string_view>>{{"run"},
                                                    {"run", "--exit-a"},
                                                    {"run", "--bogus", "f"},
                                                    {"run", "--classic"},
                                                    {"run", "--trace", "f"},
                                                    {"run", "--stats", "f"},
                                                    {"run", "a", "b"}}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm run [--classic] [--exit-a] [--max-cells N] [--] <file>") !=
              std::string::npos);
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
    const DashDir d;
    const Result r = run({"run", "--", "-malm_dash"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    // Without it a dash-leading name is an unknown option.
    CHECK(run({"run", "-malm_dash"}).code == 2);
}

TEST_CASE("run: E_ENCRYPT diagnostic and exit 1") {
    const TempFile f(">P"); // pv: Opr leaves a non-natural in the cell to encrypt
    const Result r = run({"run", f.str()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_ENCRYPT at step 1: C="));
}

TEST_CASE("run: E_WIDTH from a real program (n ratchets to 39), diagnostic and exit 1") {
    const Result r = run({"run", std::string(MALM_PROGRAMS_DIR) + "/beer99.mal"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_WIDTH at step 4258: C="));
    CHECK(r.err.find(" n=39") != std::string::npos);
    CHECK(r.err.find("(overflow)") == std::string::npos);
}

TEST_CASE("run --classic: hello world, option order, --exit-a") {
    const std::string dir = MALM_PROGRAMS_DIR;
    Result r = run({"run", "--classic", dir + "/hello.mal"});
    CHECK(r.code == 0);
    CHECK(r.out == "Hello, world.");
    CHECK(r.err.empty());

    r = run({"run", dir + "/hello.mal", "--classic"});
    CHECK(r.code == 0);
    CHECK(r.out == "Hello, world.");

    const TempFile f("u'<$_?\"J"); // classic trace program: outputs byte 222, A = 39390 at halt
    r = run({"run", "--classic", "--exit-a", f.str()}, "Q");
    CHECK(r.code == 39390 % 256);
    CHECK(r.out == "\xde");
    r = run({"run", "--classic", f.str()}, "Q");
    CHECK(r.code == 0);
}

TEST_CASE("run --classic: beer99 prints the whole song and halts") {
    const std::string dir = MALM_PROGRAMS_DIR;
    std::ifstream exp(dir + "/beer99.expected", std::ios::binary);
    const std::string expected((std::istreambuf_iterator<char>(exp)), {});
    REQUIRE(expected.size() == 11459);
    const Result r = run({"run", "--classic", dir + "/beer99.mal"});
    CHECK(r.code == 0);
    CHECK(r.out == expected);
    CHECK(r.err.empty());
}

TEST_CASE("run --classic: E_LOAD for more than 59049 cells, E_EXEC diagnostic") {
    std::string text;
    for (std::size_t p = 0; p < 59050; ++p) {
        for (int code = 33; code <= 126; ++code) {
            if (malm::core::kXlat1[(static_cast<std::size_t>(code - 33) + p) % 94] == 'o') {
                text.push_back(static_cast<char>(code));
                break;
            }
        }
    }
    const TempFile big(text);
    Result r = run({"run", "--classic", big.str()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_LOAD at byte 59049:"));

    const TempFile f("DCB");
    r = run({"run", "--classic", f.str()});
    CHECK(r.code == 1);
    CHECK(r.err == "malm: E_EXEC at step 4: C=0t10 D=0t10 A=0t0 n=10\n");
}

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string strip_ws(const std::string& s) {
    std::string r;
    for (const char c : s) {
        if (!(c == ' ' || (c >= '\t' && c <= '\r'))) {
            r.push_back(c);
        }
    }
    return r;
}

// Exports `in` to a .malm (extra args appended) and returns the file's temp path holder.
struct Exported {
    TempFile file{""};
    Result r;
};

Exported export_malm(const std::string& in, std::vector<std::string_view> extra = {}) {
    Exported e;
    std::vector<std::string_view> args{"export", "--malm", in, "-o"};
    const std::string outp = e.file.str();
    args.push_back(outp);
    args.insert(args.end(), extra.begin(), extra.end());
    e.r = run(args);
    return e;
}

} // namespace

TEST_CASE("export: usage errors exit 2") {
    const TempFile f("(P");
    const std::string in = f.str();
    for (const std::vector<std::string_view>& args : std::vector<std::vector<std::string_view>>{
             {"export"},
             {"export", in, "-o", "x"}, // neither --text nor --malm
             {"export", "--text", "--malm", in, "-o", "x"},
             {"export", "--text", "-o", "x"}, // no input
             {"export", "--text", in},        // no -o
             {"export", "--text", in, "-o"},  // -o without a value
             {"export", "--text", in, "-o", "x", "-o", "y"},
             {"export", "--text", in, "extra", "-o", "x"},
             {"export", "--text", "--bogus", in, "-o", "x"},
             {"export", "--text", "--exit-a", in, "-o", "x"},
             {"export", "--text", "--codec", "packed", in, "-o", "x"},
             {"export", "--malm", "--codec", "lz4", in, "-o", "x"},
             {"export", "--malm", in, "-o", "x", "--codec"}}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm export") != std::string::npos);
    }
}

TEST_CASE("export: bad input exits 1") {
    const TempFile f("(A");
    const TempFile o("");
    Result r = run({"export", "--malm", f.str(), "-o", o.str()});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_LOAD at byte 1: "));
    r = run({"export", "--text", "/nonexistent/malm_none", "-o", o.str()});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: cannot open"));
    r = run({"export", "--text", f.str(), "-o", "/nonexistent-dir/x"});
    CHECK(r.code == 1); // (A fails to load first
    const TempFile ok("(P");
    r = run({"export", "--text", ok.str(), "-o", "/nonexistent-dir/x"});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: cannot write '/nonexistent-dir/x'"));
}

TEST_CASE("export --malm then run matches running the text") {
    const std::string dir = MALM_PROGRAMS_DIR;
    struct Case {
        std::string file;
        bool classic;
    };
    for (const Case& c : {Case{"hello.mal", false}, Case{"hello.mal", true},
                          Case{"beer99.mal", true}, Case{"halt2.mal", false}}) {
        for (const std::string_view codec : {"packed", "zstd"}) {
            std::vector<std::string_view> extra{"--codec", codec};
            if (c.classic) {
                extra.push_back("--classic");
            }
            const Exported e = export_malm(dir + "/" + c.file, extra);
            REQUIRE_MESSAGE(e.r.code == 0, e.r.err);
            CHECK(e.r.out.empty());
            CHECK(slurp(e.file.str()).starts_with("MALM"));

            std::vector<std::string_view> text_args{"run"};
            if (c.classic) {
                text_args.push_back("--classic");
            }
            const std::string text_path = dir + "/" + c.file;
            text_args.push_back(text_path);
            const Result want = run(text_args);
            // The header's mode decides: no --classic needed for the .malm.
            const Result got = run({"run", e.file.str()});
            CHECK_MESSAGE(got.code == want.code, c.file << " " << codec);
            CHECK(got.out == want.out);
            CHECK(got.err == want.err);
        }
    }
}

TEST_CASE("run: .malm mode handling") {
    const std::string dir = MALM_PROGRAMS_DIR;
    const Exported un = export_malm(dir + "/hello.mal");
    const Exported cl = export_malm(dir + "/hello.mal", {"--classic"});
    REQUIRE(un.r.code == 0);
    REQUIRE(cl.r.code == 0);

    // --classic with an unleashed .malm is a usage error.
    Result r = run({"run", "--classic", un.file.str()});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err.find("unleashed-mode .malm") != std::string::npos);
    CHECK(r.err.find("--classic") != std::string::npos);

    // --classic with a classic .malm is fine, with or without the flag.
    for (const bool flag : {false, true}) {
        std::vector<std::string_view> args{"run"};
        if (flag) {
            args.push_back("--classic");
        }
        const std::string clp = cl.file.str();
        args.push_back(clp);
        r = run(args);
        CHECK(r.code == 0);
        CHECK(r.out == "Hello, world.");
    }

    // export with a mismatching --classic on a .malm input is the same usage error.
    const TempFile o("");
    r = run({"export", "--malm", "--classic", un.file.str(), "-o", o.str()});
    CHECK(r.code == 2);
    r = run({"export", "--text", "--classic", un.file.str(), "-o", o.str()});
    CHECK(r.code == 2);
}

TEST_CASE("run: EXIT_A comes from the .malm header; --exit-a also enables it") {
    const TempFile f("u'a%@\"K");
    const Exported plain = export_malm(f.str());
    const Exported flagged = export_malm(f.str(), {"--exit-a"});
    REQUIRE(plain.r.code == 0);
    REQUIRE(flagged.r.code == 0);

    Result r = run({"run", plain.file.str()}, "A");
    CHECK(r.code == 0);
    CHECK(r.out == "A");
    r = run({"run", "--exit-a", plain.file.str()}, "A");
    CHECK(r.code == 25);
    r = run({"run", flagged.file.str()}, "A");
    CHECK(r.code == 25);
    CHECK(r.out == "A");
    r = run({"run", "--exit-a", flagged.file.str()}, "A");
    CHECK(r.code == 25);

    // Re-exporting a flagged .malm keeps the flag.
    const Exported again = export_malm(flagged.file.str());
    REQUIRE(again.r.code == 0);
    CHECK(run({"run", again.file.str()}, "A").code == 25);
}

TEST_CASE("export --text: round trip is byte-identical after stripping whitespace") {
    const std::string dir = MALM_PROGRAMS_DIR;
    for (const char* name :
         {"hello.mal", "beer99.mal", "cat_nostop.mal", "cat5.mal", "halt2.mal"}) {
        const std::string orig = dir + "/" + name;
        const std::string want = strip_ws(slurp(orig));

        // text -> text
        const TempFile t1("");
        Result r = run({"export", "--text", orig, "-o", t1.str()});
        REQUIRE_MESSAGE(r.code == 0, r.err);
        const std::string got1 = slurp(t1.str());
        CHECK_MESSAGE(strip_ws(got1) == want, name);
        CHECK(got1 == want + "\n"); // no whitespace but one trailing newline

        // text -> .malm -> text, both codecs
        for (const std::string_view codec : {"packed", "zstd"}) {
            const Exported e = export_malm(orig, {"--codec", codec});
            REQUIRE(e.r.code == 0);
            const TempFile t2("");
            r = run({"export", "--text", e.file.str(), "-o", t2.str()});
            REQUIRE_MESSAGE(r.code == 0, r.err);
            CHECK_MESSAGE(slurp(t2.str()) == got1, name << " " << codec);
        }
    }
}

TEST_CASE("export: -o - writes to stdout; options in any order and --") {
    const std::string dir = MALM_PROGRAMS_DIR;
    const std::string orig = dir + "/hello.mal";
    Result r = run({"export", "--text", orig, "-o", "-"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == strip_ws(slurp(orig)) + "\n");

    r = run({"export", "-o", "-", "--text", "--", orig});
    CHECK(r.code == 0);
    CHECK(r.out == strip_ws(slurp(orig)) + "\n");

    // .malm to stdout, then run it from a file
    r = run({"export", orig, "--malm", "-o", "-", "--codec", "packed"});
    CHECK(r.code == 0);
    CHECK(r.out.starts_with("MALM"));
    const TempFile m(r.out);
    CHECK(run({"run", m.str()}).out == "Hello, world.");

    // After -- everything is an input name; before it a dash-leading word is an unknown option.
    const DashDir d;
    CHECK(run({"export", "--text", "-o", "-", "-malm_dash"}).code == 2);
    r = run({"export", "--text", "-o", "-", "--", "-malm_dash"});
    CHECK(r.code == 0);
    CHECK(r.out == "(P\n");
    // Options come before --; after it, -o is an input name.
    r = run({"export", "--text", "--", "-o", "-"});
    CHECK(r.code == 2);
}

TEST_CASE("export: the text input is validated with the classic limit under --classic") {
    std::string text;
    for (std::size_t p = 0; p < 59050; ++p) {
        for (int code = 33; code <= 126; ++code) {
            if (malm::core::kXlat1[(static_cast<std::size_t>(code - 33) + p) % 94] == 'o') {
                text.push_back(static_cast<char>(code));
                break;
            }
        }
    }
    const TempFile big(text);
    const TempFile o("");
    Result r = run({"export", "--malm", "--classic", big.str(), "-o", o.str()});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_LOAD at byte 59049:"));
    r = run({"export", "--malm", big.str(), "-o", o.str()});
    CHECK(r.code == 0);
    CHECK(run({"run", "--classic", o.str()}).code == 2); // unleashed container
}

TEST_CASE("run: corrupted .malm files give clear errors, exit 1") {
    const std::string dir = MALM_PROGRAMS_DIR;
    const Exported e = export_malm(dir + "/hello.mal", {"--codec", "packed"});
    REQUIRE(e.r.code == 0);
    const std::string good = slurp(e.file.str());

    const auto expect_invalid = [&](const std::string& content, std::string_view reason) {
        const TempFile f(content);
        const Result r = run({"run", f.str()});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err.starts_with("malm: E_LOAD: invalid .malm '" + f.str() + "': "));
        CHECK_MESSAGE(r.err.find(reason) != std::string::npos, r.err);
        // export reads it the same way
        const TempFile o("");
        const Result x = run({"export", "--text", f.str(), "-o", o.str()});
        CHECK(x.code == 1);
        CHECK(x.err == r.err);
    };

    std::string b = good;
    b[12] = static_cast<char>(b[12] ^ 1);
    expect_invalid(b, "CRC mismatch");
    b = good;
    b[3] = 'X'; // magic "MALX": not a .malm, loaded as text -> E_LOAD, not invalid .malm
    {
        const TempFile f(b);
        CHECK(run({"run", f.str()}).err.starts_with("malm: E_LOAD at byte 0"));
    }
    expect_invalid(good.substr(0, good.size() - 1), "truncated");
    expect_invalid(good.substr(0, 20), "truncated header");
    expect_invalid(good + "x", "trailing");
    b = good;
    b[4] = 9;
    expect_invalid(b, "unsupported format version");
}

TEST_CASE("export --text warns when it drops the mode or EXIT_A") {
    const std::string dir = MALM_PROGRAMS_DIR;
    const Exported plain = export_malm(dir + "/hello.mal");
    const Exported cl = export_malm(dir + "/hello.mal", {"--classic"});
    const Exported ea = export_malm(dir + "/hello.mal", {"--exit-a"});
    const Exported both = export_malm(dir + "/hello.mal", {"--classic", "--exit-a"});
    Result r = run({"export", "--text", plain.file.str(), "-o", "-"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    r = run({"export", "--text", cl.file.str(), "-o", "-"});
    CHECK(r.code == 0);
    CHECK(r.err == "malm: warning: plain text cannot represent classic mode; not exported\n");
    r = run({"export", "--text", ea.file.str(), "-o", "-"});
    CHECK(r.err == "malm: warning: plain text cannot represent EXIT_A; not exported\n");
    r = run({"export", "--text", both.file.str(), "-o", "-"});
    CHECK(r.err ==
          "malm: warning: plain text cannot represent classic mode or EXIT_A; not exported\n");
    // Text input never warns.
    r = run({"export", "--text", "--classic", dir + "/hello.mal", "-o", "-"});
    CHECK(r.err.empty());
}

namespace {

// A valid .malm of `cells` Nop cells as a tiny zstd RLE frame (see container_test.cpp).
std::string nop_malm_file(std::uint64_t cells, bool real_crc) {
    const std::uint64_t total = (cells * 3 + 7) / 8;
    const unsigned used = static_cast<unsigned>((cells * 3) % 8);
    const unsigned char last = static_cast<unsigned char>(used == 0 ? 0xFFu : (1u << used) - 1);
    std::string f = "\x28\xb5\x2f\xfd\xE0";
    for (int i = 0; i < 8; ++i) {
        f.push_back(static_cast<char>(total >> (8 * i)));
    }
    const auto block = [&](std::uint64_t size, unsigned char v, bool is_last) {
        const std::uint32_t h = static_cast<std::uint32_t>(size << 3) | 2u | (is_last ? 1u : 0u);
        f.push_back(static_cast<char>(h));
        f.push_back(static_cast<char>(h >> 8));
        f.push_back(static_cast<char>(h >> 16));
        f.push_back(static_cast<char>(v));
    };
    for (std::uint64_t left = total - 1; left > 0;) {
        const std::uint64_t n = std::min<std::uint64_t>(left, 131072);
        block(n, 0xFF, false);
        left -= n;
    }
    block(1, last, true);
    std::vector<std::uint8_t> packed(total, 0xFF);
    packed.back() = last;
    const std::uint32_t crc = real_crc ? malm::container::crc32(packed) : 0;
    std::string h = "MALM";
    const auto le = [&](std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            h.push_back(static_cast<char>(v >> (8 * i)));
        }
    };
    le(1, 2);
    h.push_back(0); // unleashed
    h.push_back(0); // flags
    h.push_back(1); // zstd
    le(0, 3);
    le(crc, 4);
    le(cells, 8);
    le(f.size(), 8);
    return h + f;
}

} // namespace

TEST_CASE("--max-cells: resource cap on .malm and text, validation") {
    // A small valid file claiming 1.6e9 cells (the review's decompression bomb) is rejected by the
    // default cap before anything is decompressed.
    const TempFile bomb(nop_malm_file(1'600'000'000ull, false));
    Result r = run({"run", bomb.str()});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with("malm: E_LOAD: invalid .malm '" + bomb.str() + "': program has "));
    CHECK(r.err.find("1600000000 cells, above the limit 1073741824 (use --max-cells)") !=
          std::string::npos);

    // 2M all-Nop cells load under a raised cap (and run off the end: E_EXEC), not under a lower
    // one.
    const TempFile big(nop_malm_file(2'000'000, true));
    r = run({"run", "--max-cells", "1999999", big.str()});
    CHECK(r.code == 1);
    CHECK(r.err.find("above the limit 1999999 (use --max-cells)") != std::string::npos);
    r = run({"run", "--max-cells", "2000000", big.str()});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_EXEC at step "));
    // export honours it as well
    const TempFile o("");
    r = run({"export", "--text", "--max-cells", "100", big.str(), "-o", o.str()});
    CHECK(r.code == 1);
    r = run({"export", "--text", "--max-cells", "2000000", big.str(), "-o", o.str()});
    CHECK(r.code == 0);
    CHECK(std::filesystem::file_size(o.path) == 2'000'001);

    // text input: the cap applies to the loader too
    const std::string hello = std::string(MALM_PROGRAMS_DIR) + "/hello.mal";
    r = run({"run", "--max-cells", "3", hello});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: E_LOAD at byte "));
    CHECK(r.err.find("(use --max-cells)") != std::string::npos);

    // validation: positive, numeric, at most the mode's format limit
    for (const std::vector<std::string_view>& args : std::vector<std::vector<std::string_view>>{
             {"run", "--max-cells"},
             {"run", "--max-cells", "0", "f"},
             {"run", "--max-cells", "-5", "f"},
             {"run", "--max-cells", "12x", "f"},
             {"run", "--max-cells", "", "f"},
             {"run", "--classic", "--max-cells", "59050", "f"},
             {"run", "--max-cells", "99999999999999999999999999", "f"},
             {"export", "--text", "--max-cells", "0", "f", "-o", "-"},
             {"export", "--malm", "--classic", "--max-cells", "59050", "f", "-o", "-"}}) {
        r = run(args);
        CHECK_MESSAGE(r.code == 2, args.back());
        CHECK(r.out.empty());
    }
    // classic: 59049 is fine
    const TempFile halt("(P");
    r = run({"run", "--classic", "--max-cells", "59049", halt.str()});
    CHECK(r.code == 0);
}
