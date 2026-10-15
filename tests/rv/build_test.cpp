// malm build (issues #26, #28, #29, #34-#36): the emu-mode image builder and aot mode, an RV32IM
// ELF into a pure .malm; its options and how it finds the C, Rust and Zig toolchains
// (toolchain_test.cpp compiles with the real ones, and builds Lua scripts).

#include <doctest/doctest.h>
#include <sys/stat.h>
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
#include "driver/build_cmd.hpp"
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
    // An executable shell script.
    std::string script(std::string_view name, std::string_view body) const {
        const std::string text = "#!/bin/sh\n" + std::string(body) + "\n";
        const std::string p = write(name, {text.begin(), text.end()});
        ::chmod(p.c_str(), 0755);
        return p;
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
             {"build", "--mode", "bogus", "-o", out, elf},
             {"build", "--mode"},
             {"build", "--cc", "-o", out, "a.c"}, // "-o" is taken as the compiler, then no -o
             {"build", "--cc"},
             {"build", "--exe", "-o", "-", "a.c"},
             {"build", "--exe", "a.c"},
             {"build", "-o", out, "a.c", elf},
             {"build", "-o", out, elf, "a.c"},
             {"build", "--cc", "clang", "-o", out, elf},
             {"build", "--libc", "picolibc", "-o", out, elf},
             {"build", "--libc", "glibc", "-o", out, "a.c"},
             {"build", "-o", out, "a.c", "--libc"},
             {"build", "-o", out, elf, "--", "-O0"},
             {"build", "-o", out, "--", "a.c"}, // after --, a.c is a compiler argument
             {"build", "-o", out, "a.cpp", elf},
             {"build", "--libc", "minimal", "-o", out, "a.c", "b.cc"}, // C++ needs picolibc
             {"build", "-o", out, "a.rs", "b.rs"},                     // one crate root
             {"build", "-o", out, "a.rs", "b.c"},
             {"build", "-o", out, "a.c", "b.rs"},
             {"build", "-o", out, "a.rs", elf},
             {"build", "--rustc"},
             {"build", "--rustc", "rustc", "-o", out, "a.c"},
             {"build", "--rustc", "rustc", "-o", out, elf},
             {"build", "--cc", "clang", "-o", out, "a.rs"},
             {"build", "--libc", "picolibc", "-o", out, "a.rs"},
             {"build", "-o", out, "a.zig", "b.zig"}, // one root
             {"build", "-o", out, "a.zig", "b.rs"},
             {"build", "--zig"},
             {"build", "--zig", "zig", "-o", out, "a.rs"},
             {"build", "--rustc", "rustc", "-o", out, "a.zig"},
             {"build", "--cc", "clang", "-o", out, "a.zig"},
             {"build", "-o", out, "a.lua", "b.lua"}, // one script
             {"build", "-o", out, "a.lua", "b.c"},
             {"build", "-o", out, "a.c", "b.lua"},
             {"build", "--libc", "picolibc", "-o", out, "a.lua"},
             {"build", "--rustc", "rustc", "-o", out, "a.lua"},
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
    CHECK(r.err == "malm build: " + text +
                       ": neither a C, C++, Rust or Zig source (.c, .cpp, .cc, .cxx, .rs, .zig), "
                       "a Lua script (.lua) nor an ELF executable\n");

    // Before anything is compiled.
    r = run({"build", "--mode", "aot", "-o", out, "a.cpp", "--", "-fexceptions"});
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: C++ exceptions (-fexceptions) are not supported in aot mode\n");
    r = run({"build", "--mode", "aot", "-o", out, "a.lua"});
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: Lua scripts run in emu mode only (Lua's interpreter is too large "
                   "for aot mode)\n");

    // The data segment ends at 0x210, past 512 bytes of guest RAM.
    r = run({"build", "--ram", "512", "-o", out, elf});
    CHECK(r.code == 1);
    CHECK(r.err.find("does not fit in guest RAM (512 bytes)") != std::string::npos);

    r = run({"build", "-o", elf, elf});
    CHECK(r.code == 1);
    CHECK(r.err.find("refusing to overwrite the input file") != std::string::npos);
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("build: a hand-assembled ELF prints hi in aot mode" * kSlowInDebug) {
    const TempDir dir;
    const std::string elf = dir.write("hi.elf", hi_elf());
    const std::string out = (dir.path / "hi.malm").string();
    const Result r = run({"build", "--mode", "aot", "--ram", "4K", "-o", out, elf});
    INFO(r.err);
    REQUIRE(r.code == 0);

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

TEST_CASE("build: aot mode refuses self-modifying code") {
    const TempDir dir;
    ElfSpec spec;
    spec.entry = 0x100;
    spec.segments.push_back({0x100, words({0x0000100F, li(a7, 93), kEcall}), 0}); // fence.i
    const std::string elf = dir.write("smc.elf", build_elf(spec));
    const std::string out = (dir.path / "smc.malm").string();
    const Result r = run({"build", "--mode", "aot", "--ram", "4K", "-o", out, elf});
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: " + elf +
                       ": fence.i at 0x100: self-modifying code is not supported in aot mode\n");
    CHECK(!std::filesystem::exists(out));
}

namespace {

// build_command_with, with PATH pointing at directories of fake tools.
Result build_with(std::vector<std::string_view> args, std::string_view path_env) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = malm::build_command_with(args, out, err, "/proc/self/exe", path_env);
    return {code, out.str(), err.str()};
}

} // namespace

TEST_CASE("build: finding the C toolchain") {
    const TempDir dir;
    const std::string empty = (dir.path / "empty").string();
    std::filesystem::create_directories(empty);
    const std::string src = dir.write("prog.c", {});
    const std::string out = (dir.path / "prog.malm").string();

    Result r = build_with({"-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: no C compiler: clang is not in PATH (install clang and lld, or "
                   "name the compiler with --cc)\n");

    r = build_with({"--cc", "/nonexistent/clang", "-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: compiler '/nonexistent/clang' not found or not executable\n");

    r = build_with({"--cc", "mycc", "-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: compiler 'mycc' is not in PATH\n");

    // A compiler without ld.lld next to it or in PATH.
    std::filesystem::create_directories(dir.path / "cc");
    const std::string cc = dir.script("cc/mycc", "exit 0");
    r = build_with({"--cc", "mycc", "-o", out, src}, (dir.path / "cc").string());
    CHECK(r.code == 1);
    CHECK(r.err ==
          "malm build: no linker: ld.lld is not next to " + cc + " or in PATH (install lld)\n");

    // Compilers that cannot target riscv32; without --cc the newest versioned clang is taken.
    std::filesystem::create_directories(dir.path / "bin");
    dir.script("bin/ld.lld", "exit 0");
    dir.script("bin/clang-18", "echo 'clang-18: no riscv32' >&2; exit 1");
    const std::string clang20 =
        dir.script("bin/clang-20", "echo \"error: unable to create target\" >&2; exit 1");
    const std::string bin = (dir.path / "bin").string();
    r = build_with({"-o", out, src}, empty + ":" + bin);
    CHECK(r.code == 1);
    CHECK(r.err == "error: unable to create target\nmalm build: " + clang20 +
                       " cannot compile for riscv32-unknown-elf (malm build needs clang with the "
                       "RISC-V target)\n");
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("build: finding the Rust toolchain") {
    const TempDir dir;
    const std::string empty = (dir.path / "empty").string();
    std::filesystem::create_directories(empty);
    const std::string src = dir.write("main.rs", {});
    const std::string out = (dir.path / "main.malm").string();

    Result r = build_with({"-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: no Rust compiler: rustc is not in PATH (install Rust with rustup, "
                   "or name the compiler with --rustc)\n");

    r = build_with({"--rustc", "/nonexistent/rustc", "-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: compiler '/nonexistent/rustc' not found or not executable\n");

    r = build_with({"--rustc", "myrustc", "-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: compiler 'myrustc' is not in PATH\n");

    // A rustc without the target: its library directory has no core.
    std::filesystem::create_directories(dir.path / "bin");
    std::filesystem::create_directories(dir.path / "lib");
    const std::string rustc = dir.script("bin/rustc", "echo '" + (dir.path / "lib").string() + "'");
    const std::string bin = (dir.path / "bin").string();
    r = build_with({"-o", out, src}, empty + ":" + bin);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: the Rust target riscv32im-unknown-none-elf is not installed for " +
                       rustc + " (rustup target add riscv32im-unknown-none-elf)\n");

    // One that does not know the target, and one too old for the runtime.
    dir.script("bin/rustc", "echo 'error: unknown target' >&2; exit 1");
    r = build_with({"-o", out, src}, bin);
    CHECK(r.code == 1);
    CHECK(r.err == "error: unknown target\nmalm build: " + rustc +
                       " cannot compile for riscv32im-unknown-none-elf\n");

    dir.write("lib/libcore-0123.rlib", {});
    dir.script("bin/rustc", "if [ \"$1\" = --print ]; then echo '" + (dir.path / "lib").string() +
                                "'; else echo 'error: edition 2024 is unstable' >&2; exit 1; fi");
    r = build_with({"-o", out, src}, bin);
    CHECK(r.code == 1);
    CHECK(r.err == "error: edition 2024 is unstable\nmalm build: the Rust runtime does not "
                   "compile with " +
                       rustc + " (malm build needs Rust 1.85 or newer)\n");
    CHECK(!std::filesystem::exists(out));
}

TEST_CASE("build: finding the Zig toolchain") {
    const TempDir dir;
    const std::string empty = (dir.path / "empty").string();
    std::filesystem::create_directories(empty);
    const std::string src = dir.write("main.zig", {});
    const std::string out = (dir.path / "main.malm").string();

    Result r = build_with({"-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: no Zig compiler: zig is not in PATH (install Zig 0.17, or name "
                   "the compiler with --zig)\n");

    r = build_with({"--zig", "/nonexistent/zig", "-o", out, src}, empty);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: compiler '/nonexistent/zig' not found or not executable\n");

    // Another Zig release, whose standard library the runtime may not compile with.
    std::filesystem::create_directories(dir.path / "bin");
    const std::string zig = dir.script("bin/zig", "echo 0.16.0");
    const std::string bin = (dir.path / "bin").string();
    r = build_with({"-o", out, src}, empty + ":" + bin);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: " + zig + " is Zig 0.16.0, the Zig runtime needs Zig 0.17\n");

    // Linker arguments are checked before zig compiles anything.
    dir.script("bin/zig", "if [ \"$1\" = version ]; then echo 0.17.1; else exit 1; fi");
    r = build_with({"-o", out, src, "--", "-Wl,-Map=x"}, bin);
    CHECK(r.code == 1);
    CHECK(r.err == "malm build: unsupported linker argument '-Map=x' for Zig (only "
                   "--defsym=<symbol>=<value>)\n");
    CHECK(!std::filesystem::exists(out));
}
