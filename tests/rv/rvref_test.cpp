#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "driver/cli.hpp"
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
