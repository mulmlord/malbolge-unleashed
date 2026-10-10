#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "asm/diagnostics.hpp"
#include "driver/cli.hpp"
#include "hla/parser.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;

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

// A file in a per-process temporary directory, removed with it.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::vector<std::uint8_t>& bytes, std::string_view name = "prog.elf") {
        const auto dir = std::filesystem::temp_directory_path() /
                         ("malm_rvref_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        path = dir / name;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() { std::filesystem::remove_all(path.parent_path()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string str() const { return path.string(); }
};

// Echoes one byte of stdin (or nothing at EOF), then exits with status 42.
std::vector<std::uint8_t> echo_elf() {
    ElfSpec spec;
    spec.segments.push_back({0x1000,
                             words({
                                 li(a0, 0),
                                 lui(a1, 2),
                                 li(a2, 1),
                                 li(a7, 63),
                                 kEcall, // read
                                 addi(a2, a0, 0),
                                 li(a0, 1),
                                 li(a7, 64),
                                 kEcall, // write
                                 li(a0, 42),
                                 li(a7, 93),
                                 kEcall, // exit
                             }),
                             0});
    spec.segments.push_back({0x2000, {0}, 0x10, 6});
    return build_elf(spec);
}

} // namespace

TEST_CASE("rvref: hidden from --help") {
    const Result r = run({"--help"});
    CHECK(r.code == 0);
    CHECK(r.out.find("rvref") == std::string::npos);
}

TEST_CASE("rvref: runs an ELF on stdin and stdout") {
    const TempFile elf(echo_elf());
    Result r = run({"rvref", elf.str()}, "xyz");
    CHECK(r.code == 42);
    CHECK(r.out == "x");
    CHECK(r.err.empty());
    r = run({"rvref", "--ram", "16K", "--", elf.str()}, "");
    CHECK(r.code == 42);
    CHECK(r.out.empty());
}

TEST_CASE("rvref: --trace and --regs go to stderr") {
    const TempFile elf(echo_elf());
    const Result r = run({"rvref", "--trace", "--regs", elf.str()}, "q");
    CHECK(r.code == 42);
    CHECK(r.out == "q");
    CHECK(r.err.starts_with("step=1 pc=0x00001000 insn=0x00000513\n"
                            "step=2 pc=0x00001004 insn=0x000025b7 a1=0x00002000\n"));
    CHECK(r.err.find("step=5 pc=0x00001010 insn=0x00000073 a0=0x00000001\n") != std::string::npos);
    CHECK(r.err.find("step=12 pc=0x0000102c insn=0x00000073\npc   0x0000102c\nsteps 12\n") !=
          std::string::npos);
    CHECK(r.err.find("\nx2 sp    0x00100000\n") != std::string::npos);
    CHECK(r.err.ends_with("\nx31 t6   0x00000000\n"));
}

TEST_CASE("rvref: faults and ELF errors exit 1") {
    ElfSpec spec;
    spec.segments.push_back({0x1000, words({li(a0, 1), 0}), 0});
    const TempFile bad(build_elf(spec));
    Result r = run({"rvref", bad.str()});
    CHECK(r.code == 1);
    CHECK(r.err == "malm rvref: illegal instruction 0x00000000 at pc=0x00001004 (step 2)\n");

    const TempFile text(std::vector<std::uint8_t>{'h', 'i', '\n'}, "text.elf");
    r = run({"rvref", text.str()});
    CHECK(r.code == 1);
    CHECK(r.err == "malm rvref: " + text.str() + ": not an ELF file\n");

    // The data segment at 0x2000 does not fit in 4 KiB of RAM.
    const TempFile elf(echo_elf());
    r = run({"rvref", "--ram", "4096", elf.str()});
    CHECK(r.code == 1);
    CHECK(r.err.find("does not fit in guest RAM (4096 bytes)") != std::string::npos);

    r = run({"rvref", "/nonexistent/prog.elf"});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: "));
}

TEST_CASE("rvref: usage errors exit 2") {
    for (const std::vector<std::string_view>& args : {
             std::vector<std::string_view>{"rvref"},
             {"rvref", "--ram"},
             {"rvref", "--ram", "0", "x.elf"},
             {"rvref", "--ram", "15", "x.elf"},
             {"rvref", "--ram", "1025M", "x.elf"},
             {"rvref", "--ram", "1G", "x.elf"},
             {"rvref", "--ram", "K", "x.elf"},
             {"rvref", "--ram", "-16", "x.elf"},
             {"rvref", "--bogus", "x.elf"},
             {"rvref", "a.elf", "b.elf"},
         }) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.err.find("usage: malm rvref") != std::string::npos);
    }
    // The largest RAM is accepted (and then the missing file is the error).
    CHECK(run({"rvref", "--ram", "1024M", "/nonexistent/prog.elf"}).code == 1);
}

// malm rvemu (issue #25): the HLA program of rv-emu. Compiling and running it is emu_test's job;
// here it only has to parse.

bool parses(const std::string& src) {
    malm::assembler::Diagnostics diags;
    malm::hla::parse(src, "emu.mhl", diags);
    return !diags.has_errors();
}

TEST_CASE("rvemu: hidden from --help, writes the emulator") {
    CHECK(run({"--help"}).out.find("rvemu") == std::string::npos);
    const Result r = run({"rvemu", "--ram", "4K"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out.starts_with("; rv-emu"));
    CHECK(r.out.find("ram mem[1024]\n") != std::string::npos); // zero RAM: no initial values
    CHECK(r.out.find("reg pc = 0x0,") != std::string::npos);
    CHECK(r.out.find("x2 = 0x1000") != std::string::npos);
    CHECK(r.out.find("halt 1") != std::string::npos);
    CHECK(parses(r.out));

    const Result h = run({"rvemu", "--harness", "--ram", "4K"});
    CHECK(h.code == 0);
    CHECK(h.out.find("halt 1") == std::string::npos); // stops continue at fetch
    CHECK(parses(h.out));
}

TEST_CASE("rvemu: a guest program in RAM") {
    const TempFile elf(echo_elf());
    const TempFile mhl({}, "emu.mhl");
    const Result r = run({"rvemu", "--ram", "64K", "-o", mhl.str(), "--", elf.str()});
    CHECK(r.code == 0);
    CHECK(r.out.empty());
    std::ifstream f(mhl.path);
    const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(src.find("reg pc = 0x1000,") != std::string::npos);
    CHECK(src.find("reg brk = 0x2010, brkmin = 0x2010") != std::string::npos);
    // Words 0 .. 0x3FF are zero, word 0x400 (address 0x1000) is the first instruction, the .bss
    // segment at 0x2000 adds nothing.
    const std::size_t at = src.find("ram mem[16384] = ");
    REQUIRE(at != std::string::npos);
    std::string zeros;
    for (int i = 0; i < 0x400; ++i) {
        zeros += "0x0, ";
    }
    CHECK(src.compare(at + 17, zeros.size(), zeros) == 0);
    char first[16];
    std::snprintf(first, sizeof first, "0x%X,", li(a0, 0));
    CHECK(src.compare(at + 17 + zeros.size(), std::strlen(first), first) == 0);
    CHECK(parses(src));
}

TEST_CASE("rvemu: errors") {
    CHECK(run({"rvemu", "--ram", "8M"}).code == 2); // above HLA's RAM limit
    CHECK(run({"rvemu", "--ram", "100"}).code == 2);
    CHECK(run({"rvemu", "--ram"}).code == 2);
    CHECK(run({"rvemu", "-o"}).code == 2);
    CHECK(run({"rvemu", "--bogus"}).code == 2);
    CHECK(run({"rvemu", "a.elf", "b.elf"}).code == 2);
    const Result missing = run({"rvemu", "/nonexistent/prog.elf"});
    CHECK(missing.code == 1);
    const TempFile junk({'n', 'o', 't', ' ', 'e', 'l', 'f'});
    const Result bad = run({"rvemu", junk.str()});
    CHECK(bad.code == 1);
    CHECK(bad.err.starts_with("malm rvemu: "));
    const TempFile big(echo_elf());
    CHECK(run({"rvemu", "--ram", "4K", big.str()}).code == 1); // the segments lie above 4K
}
