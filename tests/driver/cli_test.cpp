#include <doctest/doctest.h>
#include <sys/stat.h>
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
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "core/classic.hpp"
#include "core/tables.hpp"
#include "driver/cli.hpp"
#include "driver/pack_cmd.hpp"
#include "driver/packed_run.hpp"
#include "pack/trailer.hpp"

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
        if (sub == "run" || sub == "export" || sub == "pack" || sub == "asm") {
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
                                                    {"run", "--nosuch", "f"},
                                                    {"run", "--trace-from"},
                                                    {"run", "a", "b"}}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm run [--classic] [--exit-a] [--max-cells N] [--stats]") !=
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

TEST_CASE("run --classic: beer99 prints the whole song and halts" * doctest::test_suite("slow")) {
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

// One case of the export -> run comparison; `file` is relative to the committed programs.
static void check_export_then_run(const std::string& file, bool classic) {
    const std::string dir = MALM_PROGRAMS_DIR;
    for (const std::string_view codec : {"packed", "zstd"}) {
        std::vector<std::string_view> extra{"--codec", codec};
        if (classic) {
            extra.push_back("--classic");
        }
        const Exported e = export_malm(dir + "/" + file, extra);
        REQUIRE_MESSAGE(e.r.code == 0, e.r.err);
        CHECK(e.r.out.empty());
        CHECK(slurp(e.file.str()).starts_with("MALM"));

        std::vector<std::string_view> text_args{"run"};
        if (classic) {
            text_args.push_back("--classic");
        }
        const std::string text_path = dir + "/" + file;
        text_args.push_back(text_path);
        const Result want = run(text_args);
        // The header's mode decides: no --classic needed for the .malm.
        const Result got = run({"run", e.file.str()});
        CHECK_MESSAGE(got.code == want.code, file << " " << codec);
        CHECK(got.out == want.out);
        CHECK(got.err == want.err);
    }
}

TEST_CASE("export --malm then run matches running the text") {
    check_export_then_run("hello.mal", false);
    check_export_then_run("hello.mal", true);
    check_export_then_run("halt2.mal", false);
}

TEST_CASE("export --malm then run matches running the text (classic beer99)" *
          doctest::test_suite("slow")) {
    check_export_then_run("beer99.mal", true);
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

// ---- pack ----

namespace {

std::string slurp_bin(const std::filesystem::path& p) {
    // Bulk read: the packed executables are as large as the test binary (tens of MB under ASan),
    // and an istreambuf_iterator copy of that takes seconds.
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    std::string s(static_cast<std::size_t>(f.tellg()), '\0');
    f.seekg(0);
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    return s;
}

std::filesystem::path unique_path(const char* tag) {
    static int counter = 0;
    return std::filesystem::temp_directory_path() /
           (std::string("malm_pack_test_") + tag + "_" +
            std::to_string(static_cast<long>(getpid())) + "_" + std::to_string(counter++));
}

// A packed executable produced by `malm pack`; removed on destruction.
struct Packed {
    std::filesystem::path path = unique_path("exe");
    Result r;
    explicit Packed(const std::string& input, std::vector<std::string_view> opts = {}) {
        const std::string out = path.string();
        std::vector<std::string_view> args{"pack"};
        args.insert(args.end(), opts.begin(), opts.end());
        args.insert(args.end(), {"-o", out, input});
        r = run(args);
    }
    ~Packed() { std::filesystem::remove(path); }
    // Runs the embedded program the way the startup check of the packed binary does.
    Result exec(const std::string& input = "") const {
        std::istringstream in(input);
        std::ostringstream out;
        std::ostringstream err;
        const auto rc = malm::run_packed(path.c_str(), in, out, err);
        return {rc.value_or(-1), out.str(), err.str()};
    }
};

} // namespace

TEST_CASE("pack: usage errors exit 2") {
    const TempFile f("(P");
    const std::string in = f.str();
    for (const std::vector<std::string_view>& args : std::vector<std::vector<std::string_view>>{
             {"pack"},
             {"pack", in},                       // no -o
             {"pack", "-o", "x"},                // no input
             {"pack", in, "-o"},                 // -o without a value
             {"pack", in, "-o", "x", "-o", "y"}, // -o twice
             {"pack", in, "extra", "-o", "x"},   // two inputs
             {"pack", "--bogus", in, "-o", "x"}, // unknown option
             {"pack", "--codec", "zstd", in, "-o", "x"},
             {"pack", "--text", in, "-o", "x"},
             {"pack", "--max-cells", "0", in, "-o", "x"},
             {"pack", "--max-cells", in, "-o", "x"},
             {"pack", "--classic", "--max-cells", "59050", in, "-o", "x"},
             {"pack", in, "-o", "-"}}) { // binary to a terminal makes no sense
        const Result r = run(args);
        CHECK_MESSAGE(r.code == 2, args.back());
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm pack") != std::string::npos);
    }
    CHECK(run({"pack", in, "-o", "-"}).err.find("-o - is not supported") != std::string::npos);
}

TEST_CASE("pack: bad input exits 1 and writes nothing") {
    const TempFile bad("(A");
    const std::filesystem::path out = unique_path("bad");
    Result r = run({"pack", bad.str(), "-o", out.string()});
    CHECK(r.code == 1);
    CHECK(r.err.find("E_LOAD") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(out));

    r = run({"pack", "/nonexistent/in.mal", "-o", out.string()});
    CHECK(r.code == 1);
    CHECK_FALSE(std::filesystem::exists(out));

    // unwritable output directory: failure, no stray temp file
    const TempFile ok("(P");
    r = run({"pack", ok.str(), "-o", "/nonexistent-dir/out"});
    CHECK(r.code == 1);
    CHECK(r.err.find("cannot write '/nonexistent-dir/out'") != std::string::npos);
}

TEST_CASE("pack: refuses to overwrite the running binary (and its links)") {
    // Works on a temp copy standing in for the running binary, never on the real test binary.
    const TempFile f("(P");
    const std::filesystem::path fake = unique_path("self");
    std::filesystem::copy_file("/proc/self/exe", fake);
    const std::string self = fake.string();
    const std::string before = slurp_bin(fake);
    auto pack_over = [&](const std::string& target) {
        std::ostringstream err;
        const std::string in = f.str();
        const std::vector<std::string_view> args{"-o", target, in};
        const int code = malm::pack_command_with(args, err, self.c_str());
        return std::pair{code, err.str()};
    };
    auto check_refused = [&](const std::string& target, const char* what) {
        const auto [code, err] = pack_over(target);
        CHECK_MESSAGE(code == 1, what);
        CHECK_MESSAGE(err.find("refusing to overwrite the running binary") != std::string::npos,
                      what);
    };
    check_refused(self, "the binary itself");
    // a hard link (same directory, so same filesystem) and a symlink are the same file too
    const std::filesystem::path hard = unique_path("hard");
    const std::filesystem::path sym = unique_path("sym");
    std::filesystem::create_hard_link(fake, hard);
    std::filesystem::create_symlink(fake, sym);
    check_refused(hard.string(), "hard link");
    check_refused(sym.string(), "symlink");
    CHECK(slurp_bin(fake) == before);
    CHECK(std::filesystem::is_symlink(sym));
    // an unrelated target is fine
    const std::filesystem::path other = unique_path("other");
    CHECK(pack_over(other.string()).first == 0);
    // ... and -o to a symlink replaces the link, not its target
    const std::filesystem::path target = unique_path("target");
    std::ofstream(target, std::ios::binary) << "keep";
    std::filesystem::remove(sym);
    std::filesystem::create_symlink(target, sym);
    CHECK(pack_over(sym.string()).first == 0);
    CHECK_FALSE(std::filesystem::is_symlink(sym));
    CHECK(slurp_bin(target) == "keep");
    for (const auto& path : {fake, hard, sym, other, target}) {
        std::filesystem::remove(path);
    }
}

TEST_CASE("pack: refuses the dynamic loader and an input that is also the output") {
    const TempFile f("(P");
    {
        const std::filesystem::path dir = unique_path("ld");
        std::filesystem::create_directory(dir);
        const std::filesystem::path loader = dir / "ld-linux-test.so.2";
        std::filesystem::copy_file("/proc/self/exe", loader);
        std::ostringstream err;
        const std::string out = (dir / "out").string();
        const std::string in = f.str();
        const std::vector<std::string_view> args{"-o", out, in};
        CHECK(malm::pack_command_with(args, err, loader.c_str()) == 1);
        CHECK(err.str().find("dynamic loader") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(out));
        std::filesystem::remove_all(dir);
    }
    const TempFile in("(P");
    const Result r = run({"pack", in.str(), "-o", in.str()});
    CHECK(r.code == 1);
    CHECK(r.err.find("refusing to overwrite the input file") != std::string::npos);
    CHECK(slurp_bin(in.path) == "(P");
}

TEST_CASE("pack: warns when malm is dynamically linked") {
    const TempFile f("(P");
    const Packed p(f.str());
    CHECK(p.r.code == 0);
    CHECK(p.r.out.empty());
#ifdef MALM_STATIC
    CHECK(p.r.err.empty());
#else
    CHECK(p.r.err == "malm: warning: this malm is dynamically linked; the packed executable "
                     "needs the same shared libraries at run time\n");
#endif
}

TEST_CASE("pack: layout, mode and reproducibility") {
    const TempFile f("u'a%@\"K");
    const Packed a(f.str());
    const Packed b(f.str());
    REQUIRE(a.r.code == 0);
    REQUIRE(b.r.code == 0);
    const std::string bytes = slurp_bin(a.path);
    CHECK(bytes == slurp_bin(b.path));

    // [binary][payload = canonical zstd .malm][trailer]
    const std::string self = slurp_bin("/proc/self/exe");
    REQUIRE(bytes.size() > self.size() + malm::pack::kTrailerSize);
    CHECK(bytes.compare(0, self.size(), self) == 0);
    const Exported e = export_malm(f.str());
    const std::string payload = slurp_bin(e.file.path);
    CHECK(bytes.substr(self.size(), payload.size()) == payload);
    CHECK(bytes.size() == self.size() + payload.size() + malm::pack::kTrailerSize);
    CHECK(bytes.compare(bytes.size() - malm::pack::kTrailerSize, 8, "MALMEXE1") == 0);

    // executable, honouring the umask (read without changing it)
    struct stat st{};
    REQUIRE(::stat(a.path.c_str(), &st) == 0);
    const mode_t mask = ::umask(0);
    ::umask(mask);
    CHECK((st.st_mode & 0777) == (0777 & ~mask));

    // packing a .malm gives the same bytes as packing its text (the payload is canonical)
    const Packed c(e.file.str());
    REQUIRE(c.r.code == 0);
    CHECK(slurp_bin(c.path) == bytes);
    // packing over an existing file replaces it
    const Result again = run({"pack", f.str(), "-o", a.path.string()});
    CHECK(again.code == 0);
    CHECK(slurp_bin(a.path) == bytes);
}

TEST_CASE("packed executable: runs the embedded program, exit status as malm run") {
    const TempFile f("u'a%@\"K");
    {
        const Packed p(f.str());
        REQUIRE(p.r.code == 0);
        const Result r = p.exec("A");
        CHECK(r.code == 0);
        CHECK(r.out == "A");
        CHECK(r.err.empty());
    }
    {
        const Packed p(f.str(), {"--exit-a"});
        REQUIRE(p.r.code == 0);
        const Result r = p.exec("A");
        CHECK(r.code == 25);
        CHECK(r.out == "A");
    }
    {
        // classic mode comes from the header
        const std::string hello = std::string(MALM_PROGRAMS_DIR) + "/hello.mal";
        const Packed p(hello, {"--classic"});
        REQUIRE(p.r.code == 0);
        const Result r = p.exec();
        CHECK(r.code == 0);
        CHECK(r.out == "Hello, world.");
    }
    {
        const TempFile fault("DCB");
        const Packed p(fault.str());
        REQUIRE(p.r.code == 0);
        const Result r = p.exec();
        CHECK(r.code == 1);
        CHECK(r.err.starts_with("malm: E_EXEC at step 4: C="));
    }
    // an ordinary file (here: the test binary itself) is not a packed executable
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    CHECK_FALSE(malm::run_packed("/proc/self/exe", in, out, err).has_value());
    CHECK_FALSE(malm::run_packed("/nonexistent", in, out, err).has_value());
}

TEST_CASE("packed executable: an unreadable one falls back to the CLI with a hint") {
    const TempFile f("(P");
    const Packed p(f.str());
    REQUIRE(p.r.code == 0);
    REQUIRE(::chmod(p.path.c_str(), 0111) == 0);
    if (::access(p.path.c_str(), R_OK) == 0) {
        MESSAGE("skipped: running with privileges that ignore file modes");
        return;
    }
    const Result r = p.exec();
    CHECK(r.code == -1); // not recognised as packed: the normal CLI would run
    CHECK(r.err.starts_with("malm: note: cannot read own executable ("));
    CHECK(r.err.find("must be readable") != std::string::npos);
}

TEST_CASE("packed executable: corrupt trailer or payload exits 1") {
    const TempFile f("u'a%@\"K");
    const Packed p(f.str());
    REQUIRE(p.r.code == 0);
    const std::string good = slurp_bin(p.path);
    const std::size_t trailer_at = good.size() - malm::pack::kTrailerSize;

    auto check_corrupt = [&](const std::string& bytes, const char* what) {
        const std::filesystem::path path = unique_path("corrupt");
        std::ofstream(path, std::ios::binary) << bytes;
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        const auto rc = malm::run_packed(path.c_str(), in, out, err);
        std::filesystem::remove(path);
        CHECK_MESSAGE(rc == 1, what);
        CHECK_MESSAGE(err.str().starts_with("malm: corrupt packed executable: "), what);
        CHECK(out.str().empty());
    };
    std::string t = good;
    t[trailer_at + 8] = static_cast<char>(t[trailer_at + 8] + 1); // offset field
    check_corrupt(t, "offset edited");
    t = good;
    t[trailer_at + 16] = static_cast<char>(t[trailer_at + 16] + 1); // length field
    check_corrupt(t, "length edited");
    t = good;
    for (std::size_t i = 8; i < 16; ++i) {
        t[trailer_at + i] = 0; // offset 0
    }
    check_corrupt(t, "offset 0");
    // a byte lost from the payload: the sizes no longer add up
    check_corrupt(good.substr(0, trailer_at - 5) + good.substr(trailer_at - 4), "byte removed");
    // a damaged payload (consistent trailer): read_malm rejects it
    t = good;
    t[trailer_at - 3] = static_cast<char>(t[trailer_at - 3] ^ 0x55);
    check_corrupt(t, "payload flipped");
}

// ---- malm asm ----

namespace {

constexpr std::string_view kAsmOk = ".CODE\n"
                                    "J: Jmp\n"
                                    "\n"
                                    ".DATA\n"
                                    "ENTRY: J 'a'<<1 (-1)\n";

// A directory under the temp dir, removed on destruction; files are given relative to it.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("malm_asm_test_" + std::to_string(static_cast<long>(getpid())) + "_" +
                std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string write(const std::string& name, const std::string& content) const {
        std::filesystem::create_directories((path / name).parent_path());
        std::ofstream(path / name, std::ios::binary) << content;
        return (path / name).string();
    }
};

} // namespace

TEST_CASE("asm --check: a valid file prints nothing and exits 0") {
    TempDir d;
    const std::string f = d.write("ok.mua", std::string(kAsmOk));
    const Result r = run({"asm", "--check", f});
    CHECK(r.code == 0);
    CHECK(r.out.empty());
    CHECK(r.err.empty());
}

TEST_CASE("asm --check: diagnostics go to stderr and the exit code is 1") {
    TempDir d;
    const std::string f = d.write("bad.mua", ".CODE\nA: Foo\nB: Rot %\n");
    const Result r = run({"asm", "--check", f});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == f + ":2:4: error: expected a command name, found 'Foo'\n" + f +
                       ":3:8: error: unexpected character '%'\n");
}

TEST_CASE("asm --check: static check errors, with notes") {
    TempDir d;
    const std::string f = d.write("dup.mua", ".CODE\nJ: Jmp\nJ: Hlt\n.DATA\nENTRY: J\n");
    const Result r = run({"asm", "--check", f});
    CHECK(r.code == 1);
    CHECK(r.err ==
          f + ":3:1: error: duplicate label 'J'\n" + f + ":2:1: note: first defined here\n");
}

TEST_CASE("asm --check: includes are read relative to the including file") {
    TempDir d;
    d.write("lib/units.mua", ".CODE\nJ: Jmp\n");
    const std::string f = d.write("main.mua", ".INCLUDE \"lib/units.mua\"\n.DATA\nENTRY: J\n");
    CHECK(run({"asm", "--check", f}).code == 0);
    d.write("lib/units.mua", ".CODE\nJ: Foo\n");
    const Result r = run({"asm", "--check", f});
    CHECK(r.code == 1);
    const std::string lib = (d.path / "lib/units.mua").string();
    CHECK(r.err == lib + ":2:4: error: expected a command name, found 'Foo'\n" + f +
                       ":1:1: note: included from here\n");
}

TEST_CASE("asm --check: a missing file is a file error, exit 1") {
    TempDir d;
    const std::string f = (d.path / "nope.mua").string();
    const Result r = run({"asm", "--check", f});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == f + ":1:1: error: cannot open '" + f + "': No such file or directory\n");
}

TEST_CASE("asm --print: canonical source on stdout after the checks") {
    TempDir d;
    const std::string f = d.write("ok.mua", std::string(kAsmOk));
    const Result r = run({"asm", "--print", f});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == ".CODE\nJ:      Jmp\n\n.DATA\nENTRY:  J 'a'<<1 (-1)\n");
    // the output is itself a valid program that prints to the same text
    const std::string g = d.write("again.mua", r.out);
    const Result r2 = run({"asm", "--print", g});
    CHECK(r2.code == 0);
    CHECK(r2.out == r.out);
}

TEST_CASE("asm --print: nothing on stdout when the checks fail") {
    TempDir d;
    const std::string f = d.write("noentry.mua", ".CODE\nJ: Jmp\n");
    const Result r = run({"asm", "--print", f});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == f + ":1:1: error: missing ENTRY: the program must define the label ENTRY in a "
                       ".DATA block\n");
}

TEST_CASE("asm --check --print together, and -- ends the options") {
    TempDir d;
    const std::string f = d.write("ok.mua", std::string(kAsmOk));
    CHECK(run({"asm", "--check", "--print", f}).code == 0);
    const Result r = run({"asm", "--print", "--", f});
    CHECK(r.code == 0);
    CHECK(!r.out.empty());
}

namespace {

// The hello world of docs/asm.md 10.1, shortened.
constexpr std::string_view kAsmHi = ".CODE\n"
                                    "ROT: Rot/Nop\n Jmp\n\n"
                                    "OUT: Out/Nop\n Jmp\n\n"
                                    "HALT: Hlt\n\n"
                                    ".DATA\n"
                                    "ENTRY: ROT 'H'<<1 R_ROT OUT ?- R_OUT\n"
                                    "       ROT 'i'<<1 R_ROT OUT ?- R_OUT HALT\n";

} // namespace

TEST_CASE("asm -o: writes a .malm that passes the strict loader and runs") {
    TempDir d;
    const std::string f = d.write("hi.mua", std::string(kAsmHi));
    const std::string out = (d.path / "hi.malm").string();
    Result r = run({"asm", f, "-o", out});
    REQUIRE_MESSAGE(r.code == 0, r.err);
    CHECK(r.out.empty());
    CHECK(r.err.empty());
    const std::string malm = slurp(out);
    CHECK(malm.starts_with("MALM"));
    r = run({"run", out});
    CHECK(r.code == 0);
    CHECK(r.out == "Hi");

    // --text gives the same program as plain text (one trailing newline), and the .malm holds
    // exactly that program: nothing but the cells, unleashed, no EXIT_A.
    r = run({"asm", "--text", f, "-o", "-"});
    REQUIRE(r.code == 0);
    CHECK(r.out.ends_with("\n"));
    CHECK(r.out.find_first_of(" \t\r") == std::string::npos);
    CHECK(r.out.starts_with("b")); // cell 0: the startup Jmp
    CHECK(run({"export", "--text", out, "-o", "-"}).out == r.out);
    const TempFile text(r.out);
    CHECK(run({"run", text.str()}).out == "Hi");
    CHECK(run({"export", "--malm", text.str(), "-o", "-"}).out == malm);

    // -o - writes the .malm to stdout; assembling is deterministic.
    r = run({"asm", "-o", "-", "--", f});
    CHECK(r.code == 0);
    CHECK(r.out == malm);
}

TEST_CASE("asm --map: one line per label, by address") {
    TempDir d;
    const std::string f = d.write("hi.mua", std::string(kAsmHi));
    const std::string out = (d.path / "hi.malm").string();
    const std::string map = (d.path / "hi.map").string();
    REQUIRE(run({"asm", "--map", map, f, "-o", out}).code == 0);
    std::istringstream lines(slurp(map));
    std::vector<std::string> names;
    std::string name;
    std::string addr;
    std::string kind;
    std::string first;
    std::string last;
    while (lines >> name >> addr >> kind >> first >> last) {
        names.push_back(name);
        CHECK(addr.starts_with("0t"));
        CHECK((kind == "code" || kind == "data"));
        CHECK(((name == "ENTRY") == (kind == "data")));
        if (name == "ENTRY") {
            CHECK(first == addr);
            CHECK(last != first);
        }
    }
    std::vector<std::string> sorted = names;
    std::ranges::sort(sorted);
    CHECK(sorted == std::vector<std::string>{"ENTRY", "HALT", "OUT", "ROT"});
}

TEST_CASE("asm: assembly errors are diagnostics, exit 1, and no output file") {
    TempDir d;
    const std::string f = d.write("wide.mua", ".WIDTH 10\n.CODE\nJ: Jmp\n.DATA\nENTRY: J 1t1 " +
                                                  std::string("0t") + std::string(11, '1') + "\n");
    const std::string out = (d.path / "wide.malm").string();
    const Result r = run({"asm", f, "-o", out});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err.starts_with(f + ":"));
    CHECK(r.err.find(": error: ") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(out));
}

TEST_CASE("asm: refuses to overwrite the input file") {
    TempDir d;
    const std::string f = d.write("hi.mua", std::string(kAsmHi));
    for (const auto& args : std::vector<std::vector<std::string_view>>{
             {"asm", f, "-o", f}, {"asm", f, "-o", "-", "--map", f}}) {
        const Result r = run(args);
        CHECK(r.code == 1);
        CHECK(r.err.find("refusing to overwrite the input file") != std::string::npos);
    }
    CHECK(slurp(f) == kAsmHi);
}

TEST_CASE("asm: usage errors exit 2") {
    for (const auto& args :
         std::vector<std::vector<std::string_view>>{{"asm"},
                                                    {"asm", "--check"},
                                                    {"asm", "a.mua"},
                                                    {"asm", "--text", "a.mua"},
                                                    {"asm", "--map", "m", "a.mua"},
                                                    {"asm", "a.mua", "-o", "x", "-o", "y"},
                                                    {"asm", "a.mua", "-o", "x", "--map", "-"},
                                                    {"asm", "a.mua", "-o", "x", "--map", "x"},
                                                    {"asm", "--print", "a.mua", "b.mua"},
                                                    {"asm", "--bogus", "a.mua"},
                                                    {"asm", "--check", "-o"},
                                                    {"asm", "--check", "-o", "x", "a.mua"},
                                                    {"asm", "--check", "--text", "a.mua"}}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err.find("usage: malm asm") != std::string::npos);
    }
}

TEST_CASE("asm: a dash-leading file name works after --") {
    TempDir d;
    d.write("-odd.mua", std::string(kAsmOk));
    const auto old = std::filesystem::current_path();
    std::filesystem::current_path(d.path);
    const Result r = run({"asm", "--check", "--", "-odd.mua"});
    std::filesystem::current_path(old);
    CHECK(r.code == 0);
}
