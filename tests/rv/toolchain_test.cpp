// malm build's C toolchain (issue #28): compile_c with the real clang and ld.lld and the runtime
// built into malm, the ELF run on the reference emulator. Fast, also in the debug build: no
// emulator is compiled.

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "driver/toolchain.hpp"
#include "rv/elf.hpp"
#include "rv/machine.hpp"
#include "rv_test_util.hpp"

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("malm_toolchain_test_" + std::to_string(::getpid()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string write(std::string_view name, std::string_view text) const {
        const auto p = path / name;
        std::ofstream(p) << text;
        return p.string();
    }
};

struct Compiled {
    bool ok = false;
    std::string error; // compile_c's one-line error
    std::string diags; // the tools' output
    std::vector<std::uint8_t> elf;
};

Compiled compile(std::vector<std::string_view> sources, std::vector<std::string_view> args = {}) {
    const auto tools = malm::find_toolchain(MALM_RV_CC, "");
    REQUIRE(tools.has_value());
    std::ostringstream diags;
    auto elf = malm::compile_c(*tools, sources, args, diags);
    Compiled c;
    c.ok = elf.has_value();
    c.diags = diags.str();
    if (c.ok) {
        c.elf = std::move(*elf);
    } else {
        c.error = elf.error();
    }
    return c;
}

// Runs the ELF on the reference emulator with 1M of guest RAM (sort needs a heap).
malm::rv::Stop run(const std::vector<std::uint8_t>& elf, rvtest::StringIo& io) {
    constexpr std::uint32_t kRam = 1024 * 1024;
    const auto image = malm::rv::load_elf(elf, kRam);
    REQUIRE(image.has_value());
    malm::rv::Machine m(*image, kRam);
    return m.run(io);
}

} // namespace

TEST_CASE("toolchain: ld.lld is found next to the compiler") {
    const auto tools = malm::find_toolchain(MALM_RV_CC, "");
    REQUIRE(tools.has_value());
    CHECK(tools->cc == MALM_RV_CC);
    CHECK(std::filesystem::path(tools->ld).filename() == "ld.lld");
}

TEST_CASE("toolchain: the C examples on the runtime") {
    const Compiled hello = compile({MALM_EXAMPLES_DIR "/hello.c"});
    REQUIRE(hello.ok);
    CHECK(hello.diags.empty()); // the runtime compiles quietly
    rvtest::StringIo io;
    malm::rv::Stop stop = run(hello.elf, io);
    CHECK(stop.kind == malm::rv::Stop::Kind::Exit);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "Hello, world!\n");

    const Compiled sort = compile({MALM_EXAMPLES_DIR "/sort.c"});
    REQUIRE(sort.ok);
    io = {};
    io.in = "pear\napple\n\nfig";
    stop = run(sort.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "\napple\nfig\npear\n");
}

TEST_CASE("toolchain: several sources, compiler and linker arguments") {
    const TempDir dir;
    const std::string main_c = dir.write("main.c", R"(#include <stdint.h>
#include <stdio.h>
int twice(int);
extern char answer[]; // from -Wl,--defsym
int main(void) {
    printf("%d %s\n", twice(VALUE), LEVEL);
    return (int)(uintptr_t)answer;
}
)");
    const std::string twice_c = dir.write("twice.c", "int twice(int x) { return 2 * x; }\n");
    const Compiled c =
        compile({main_c, twice_c}, {"-DVALUE=21", "-DLEVEL=\"x\"", "-Wl,--defsym=answer=42"});
    INFO(c.diags);
    REQUIRE(c.ok);
    rvtest::StringIo io;
    const malm::rv::Stop stop = run(c.elf, io);
    CHECK(io.out == "42 x\n");
    CHECK(stop.exit_code == 42);
}

TEST_CASE("toolchain: compiler and linker errors") {
    const TempDir dir;
    const std::string bad = dir.write("bad.c", "int main(void) { return x; }\n");
    Compiled c = compile({bad});
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
    CHECK(c.diags.find(bad + ":1:") != std::string::npos);

    // Warnings of the program are shown, and its arguments come after the default -O2 and
    // can turn them into errors.
    const std::string warn = dir.write("warn.c", "int main(void) { int unused; return 0; }\n");
    c = compile({warn}, {"-Wall"});
    CHECK(c.ok);
    CHECK(c.diags.find("unused") != std::string::npos);
    c = compile({warn}, {"-Wall", "-Werror"});
    CHECK(!c.ok);

    const std::string undef = dir.write("undef.c", "void f(void); int main(void) { f(); }\n");
    c = compile({undef});
    CHECK(!c.ok);
    CHECK(c.error == "linking failed");
    CHECK(c.diags.find("undefined symbol: f") != std::string::npos);

    c = compile({(dir.path / "missing.c").string()});
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
}
