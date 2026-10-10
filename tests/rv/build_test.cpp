// malm build (issue #26): the emu-mode image builder, an RV32IM ELF into a pure .malm.

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "core/vm.hpp"
#include "driver/cli.hpp"
#include "driver/program_io.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;

namespace {

// A debug build has sanitizers, and its assembler and VM are ~70x slower: compiling the emulator
// takes it minutes, so there the end-to-end case is slow (nightly). The literal suite name is what
// malm_add_test looks for to register the slow cases.
#ifdef NDEBUG
const auto kSlowInDebug = doctest::test_suite("");
#else
const auto kSlowInDebug = doctest::test_suite("slow");
#endif

struct Result {
    int code;
    std::string out;
    std::string err;
};

Result run(std::vector<std::string_view> args) {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    const int code = malm::run_cli(args, in, out, err);
    return {code, out.str(), err.str()};
}

// A per-test temporary directory, removed with it.
struct TempDir {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("malm_build_test_" + std::to_string(::getpid()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string write(std::string_view name, const std::vector<std::uint8_t>& bytes) const {
        const auto p = path / name;
        std::ofstream(p, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return p.string();
    }
    std::vector<std::uint8_t> read(std::string_view name) const {
        std::ifstream f(path / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }
};

// Hand-assembled: write(1, "hi", 2), then exit(7). The text at 0x100, the string in a data
// segment of its own at 0x200 with .bss after it.
std::vector<std::uint8_t> hi_elf() {
    ElfSpec spec;
    spec.entry = 0x100;
    spec.segments.push_back({0x100,
                             words({
                                 li(a0, 1),
                                 li(a1, 0x200),
                                 li(a2, 2),
                                 li(a7, 64),
                                 kEcall, // write
                                 li(a0, 7),
                                 li(a7, 93),
                                 kEcall, // exit
                             }),
                             0});
    spec.segments.push_back({0x200, {'h', 'i'}, 0x10, 6});
    return build_elf(spec);
}

} // namespace

TEST_CASE("build: a hand-assembled ELF prints hi in emu mode" * kSlowInDebug) {
    const TempDir dir;
    const std::string elf = dir.write("hi.elf", hi_elf());
    const std::string out = (dir.path / "hi.malm").string();
    const Result r = run({"build", "--ram", "4K", "-o", out, elf});
    INFO(r.err);
    REQUIRE(r.code == 0);
    CHECK(r.out.empty());

    // Pure: the text passes the strict loader unchanged, and the .malm holds it and nothing else.
    const std::vector<std::uint8_t> malm = dir.read("hi.malm");
    const auto program = malm::container::read_malm(malm);
    REQUIRE(program.has_value());
    CHECK(program->exit_a);
    CHECK(malm::is_pure(program->cells, malm, true));

    malm::core::Vm vm(program->cells);
    StringIo io;
    const auto res = vm.run(io, 200'000'000);
    REQUIRE(res.kind == malm::core::RunResult::Kind::Halted);
    CHECK(io.out == "hi");
    CHECK(malm::core::exit_status(res, vm, program->exit_a) == 7);
}

TEST_CASE("build: errors") {
    const TempDir dir;
    const std::string elf = dir.write("hi.elf", hi_elf());
    const std::string out = (dir.path / "out.malm").string();

    for (const std::vector<std::string_view>& args : {
             std::vector<std::string_view>{"build"},
             {"build", "-o", out},
             {"build", elf},
             {"build", "-o"},
             {"build", "-o", out, "-o", out, elf},
             {"build", "--ram", "8M", "-o", out, elf}, // above HLA's RAM limit
             {"build", "--ram", "100", "-o", out, elf},
             {"build", "--ram"},
             {"build", "--bogus", "-o", out, elf},
             {"build", "-o", out, elf, elf},
         }) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(r.err.find("usage: malm build") != std::string::npos);
    }

    Result r = run({"build", "-o", out, "/nonexistent/prog.elf"});
    CHECK(r.code == 1);
    CHECK(r.err.starts_with("malm: "));

    const std::string text = dir.write("hello.mal", {'(', '=', '<', '`'});
    r = run({"build", "-o", out, text});
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: " + text + ": not an ELF file\n");

    // The data segment ends at 0x210, past 512 bytes of guest RAM.
    r = run({"build", "--ram", "512", "-o", out, elf});
    CHECK(r.code == 1);
    CHECK(r.err.find("does not fit in guest RAM (512 bytes)") != std::string::npos);

    r = run({"build", "-o", elf, elf});
    CHECK(r.code == 1);
    CHECK(r.err.find("refusing to overwrite the input file") != std::string::npos);
    CHECK(!std::filesystem::exists(out));
}
