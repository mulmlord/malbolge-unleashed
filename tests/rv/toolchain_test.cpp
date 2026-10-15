// malm build's C, C++, Rust and Zig toolchains (issues #28, #32-#35): compile_c with the real
// clang and ld.lld and the runtime built into malm, compile_rust and compile_zig with the real
// rustc and zig and their runtimes, the ELF run on the reference emulator. Fast, also in the debug
// build: no emulator is compiled.

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "driver/runtime_files.hpp"
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

Compiled compile(std::vector<std::string_view> sources, std::vector<std::string_view> args = {},
                 malm::Libc libc = malm::Libc::Minimal) {
    const auto tools = malm::find_toolchain(MALM_RV_CC, "");
    REQUIRE(tools.has_value());
    std::ostringstream diags;
    auto elf = malm::compile_c(*tools, sources, args, diags, libc);
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

TEST_CASE("toolchain: picolibc") {
    if (malm::picolibc_sysroot().empty()) {
        MESSAGE("malm is built without picolibc (MALM_PICOLIBC)");
        return;
    }
    const Compiled stats = compile({MALM_EXAMPLES_DIR "/stats.c"}, {}, malm::Libc::Picolibc);
    INFO(stats.diags);
    REQUIRE(stats.ok);
    CHECK(stats.diags.empty());
    rvtest::StringIo io;
    io.in = "2 4 4 4 5 5 7 9";
    malm::rv::Stop stop = run(stats.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "count 8\nmin 2\nmax 9\nmean 5\nstddev 2\n");

    // The minimal C library has no math.h.
    const Compiled minimal = compile({MALM_EXAMPLES_DIR "/stats.c"});
    CHECK(!minimal.ok);

    // Linker arguments reach the link with picolibc too.
    const TempDir dir;
    const std::string main_c = dir.write("main.c", R"(#include <stdint.h>
#include <stdio.h>
extern char answer[];
int main(void) {
    printf("%.2f\n", (double)(uintptr_t)answer / 8);
    return (int)(uintptr_t)answer;
}
)");
    const Compiled c = compile({main_c}, {"-Wl,--defsym=answer=42"}, malm::Libc::Picolibc);
    INFO(c.diags);
    REQUIRE(c.ok);
    io = {};
    stop = run(c.elf, io);
    CHECK(io.out == "5.25\n");
    CHECK(stop.exit_code == 42);
}

TEST_CASE("toolchain: C++") {
    if (malm::picolibc_sysroot().empty()) {
        MESSAGE("malm is built without picolibc (MALM_PICOLIBC)");
        return;
    }
    const Compiled sort = compile({MALM_CXX_EXAMPLES_DIR "/sort.cpp"}, {}, malm::Libc::Picolibc);
    INFO(sort.diags);
    REQUIRE(sort.ok);
    CHECK(sort.diags.empty());
    rvtest::StringIo io;
    io.in = "pear\napple\n\nfig";
    malm::rv::Stop stop = run(sort.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "\napple\nfig\npear\n");

    // libc++'s self-check, as malm build compiles it, and with exceptions and RTTI, which link
    // the libraries built with exceptions.
    for (const std::vector<std::string_view>& args :
         {std::vector<std::string_view>{"-std=c++20"}, {"-std=c++20", "-fexceptions", "-frtti"}}) {
        const Compiled self =
            compile({MALM_RV_PROGRAMS_DIR "/cxx_test.cpp"}, args, malm::Libc::Picolibc);
        INFO(self.diags);
        REQUIRE(self.ok);
        io = {};
        stop = run(self.elf, io);
        INFO(io.out);
        CHECK(stop.exit_code == 42);
        CHECK(io.out.starts_with("cxx_test: "));
    }

    // C and C++ sources together; a throw needs -fexceptions.
    const TempDir dir;
    const std::string main_cpp = dir.write("main.cpp", R"(#include <cstdio>
#include <stdexcept>
#include <typeinfo>
extern "C" int twice(int);
int main() {
    try {
        throw std::runtime_error("thrown");
    } catch (const std::exception& e) {
        std::printf("%s %d %s\n", e.what(), twice(21), typeid(e).name());
    }
}
)");
    const std::string twice_c = dir.write("twice.c", "int twice(int x) { return 2 * x; }\n");
    Compiled c = compile({main_cpp, twice_c}, {"-fexceptions", "-frtti"}, malm::Libc::Picolibc);
    INFO(c.diags);
    REQUIRE(c.ok);
    io = {};
    stop = run(c.elf, io);
    CHECK(io.out == "thrown 42 St13runtime_error\n");
    CHECK(stop.exit_code == 0);

    c = compile({main_cpp, twice_c}, {}, malm::Libc::Picolibc);
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
    CHECK(c.diags.find("exceptions") != std::string::npos);

    c = compile({main_cpp});
    CHECK(!c.ok);
    CHECK(c.error == "C++ needs picolibc (--libc picolibc)");
}

namespace {

Compiled compile_rust(std::string_view source, std::vector<std::string_view> args = {}) {
    std::ostringstream diags;
    auto elf = malm::compile_rust(MALM_RV_RUSTC, source, args, diags);
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

} // namespace

TEST_CASE("toolchain: Rust") {
    if (std::string_view(MALM_RV_RUSTC).empty()) {
        MESSAGE("no rustc with the riscv32im target (MALM_RUST_TESTS)");
        return;
    }
    const Compiled hello = compile_rust(MALM_RUST_EXAMPLES_DIR "/hello.rs");
    INFO(hello.diags);
    REQUIRE(hello.ok);
    CHECK(hello.diags.empty());
    rvtest::StringIo io;
    malm::rv::Stop stop = run(hello.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "Hello, world!\n");

    const Compiled sort = compile_rust(MALM_RUST_EXAMPLES_DIR "/sort.rs");
    REQUIRE(sort.ok);
    io = {};
    io.in = "pear\napple\n\nfig";
    stop = run(sort.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "\napple\nfig\npear\n");

    const Compiled self = compile_rust(MALM_RV_PROGRAMS_DIR "/rust_test.rs");
    INFO(self.diags);
    REQUIRE(self.ok);
    io = {};
    stop = run(self.elf, io);
    INFO(io.out);
    CHECK(stop.exit_code == 42);
    CHECK(io.out.starts_with("rust_test: "));

    // Modules next to the crate root, an edition of the program's own, rustc and linker
    // arguments.
    const TempDir dir;
    const std::string main_rs = dir.write("main.rs", R"(#![no_std]
#![no_main]
mod twice;
use malm::prelude::*;
malm::entry!(main);
unsafe extern "C" {
    static answer: u8; // from --defsym
}
fn main() -> malm::process::ExitCode {
    let gen = "2021"; // a reserved keyword in edition 2024
    println!("{} {} {}", twice::twice(21), gen, LEVEL);
    malm::process::ExitCode::from(&raw const answer as usize as u8)
}
const LEVEL: &str = if cfg!(level = "x") { "x" } else { "?" };
)");
    dir.write("twice.rs", "pub fn twice(x: u32) -> u32 { 2 * x }\n");
    Compiled c = compile_rust(
        main_rs, {"--edition", "2021", "--cfg", "level=\"x\"", "-Wl,--defsym=answer=42"});
    INFO(c.diags);
    REQUIRE(c.ok);
    io = {};
    stop = run(c.elf, io);
    CHECK(io.out == "42 2021 x\n");
    CHECK(stop.exit_code == 42);

    // Errors: the program's, the linker's; warnings are shown.
    const std::string bad = dir.write("bad.rs", "#![no_std]\n#![no_main]\nfn f() -> u32 { x }\n");
    c = compile_rust(bad);
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
    CHECK(c.diags.find("bad.rs:3:") != std::string::npos);

    const std::string warn = dir.write("warn.rs", R"(#![no_std]
#![no_main]
malm::entry!(main);
fn main() { let unused = 1; }
)");
    c = compile_rust(warn);
    CHECK(c.ok);
    CHECK(c.diags.find("unused") != std::string::npos);
    c = compile_rust(warn, {"-D", "warnings"});
    CHECK(!c.ok);

    const std::string undef = dir.write("undef.rs", R"(#![no_std]
#![no_main]
malm::entry!(main);
unsafe extern "C" {
    fn f();
}
fn main() { unsafe { f() } }
)");
    c = compile_rust(undef);
    CHECK(!c.ok);
    CHECK(c.error == "linking failed");
    CHECK(c.diags.find("undefined symbol: f") != std::string::npos);

    c = compile_rust((dir.path / "missing.rs").string());
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
}

namespace {

Compiled compile_zig(std::string_view source, std::vector<std::string_view> args = {}) {
    std::ostringstream diags;
    auto elf = malm::compile_zig(MALM_RV_ZIG, source, args, diags);
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

} // namespace

TEST_CASE("toolchain: Zig") {
    if (std::string_view(MALM_RV_ZIG).empty()) {
        MESSAGE("no zig 0.17 (MALM_ZIG_TESTS)");
        return;
    }
    const Compiled hello = compile_zig(MALM_ZIG_EXAMPLES_DIR "/hello.zig");
    INFO(hello.diags);
    REQUIRE(hello.ok);
    CHECK(hello.diags.empty());
    rvtest::StringIo io;
    malm::rv::Stop stop = run(hello.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "Hello, world!\n");

    const Compiled sort = compile_zig(MALM_ZIG_EXAMPLES_DIR "/sort.zig");
    REQUIRE(sort.ok);
    io = {};
    io.in = "pear\napple\n\nfig";
    stop = run(sort.elf, io);
    CHECK(stop.exit_code == 0);
    CHECK(io.out == "\napple\nfig\npear\n");

    const Compiled self = compile_zig(MALM_RV_PROGRAMS_DIR "/zig_test.zig");
    INFO(self.diags);
    REQUIRE(self.ok);
    io = {};
    stop = run(self.elf, io);
    INFO(io.out);
    CHECK(stop.exit_code == 42);
    CHECK(io.out.starts_with("zig_test: "));

    // Files imported next to the root, zig arguments for every module, symbols from --defsym.
    const TempDir dir;
    const std::string main_zig = dir.write("main.zig", R"(const malm = @import("malm");
const twice = @import("twice.zig");
extern const answer: u8;
pub fn main() u8 {
    malm.print("{d} {s}\n", .{ twice.twice(21), @tagName(@import("builtin").mode) });
    return @intCast(@intFromPtr(&answer));
}
)");
    dir.write("twice.zig", "pub fn twice(x: u32) u32 {\n    return 2 * x;\n}\n");
    Compiled c = compile_zig(main_zig, {"-O", "ReleaseSmall", "-Wl,--defsym=answer=42"});
    INFO(c.diags);
    REQUIRE(c.ok);
    io = {};
    stop = run(c.elf, io);
    CHECK(io.out == "42 small\n"); // builtin.mode, as -O ReleaseSmall set it
    CHECK(stop.exit_code == 42);

    // Errors: the program's, the linker's, linker arguments zig cannot pass.
    const std::string bad = dir.write("bad.zig", "pub fn main() void {\n    x();\n}\n");
    c = compile_zig(bad);
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
    CHECK(c.diags.find("bad.zig:2:") != std::string::npos);

    const std::string undef = dir.write("undef.zig", R"(extern fn f() void;
pub fn main() void {
    f();
}
)");
    c = compile_zig(undef);
    CHECK(!c.ok);
    CHECK(c.error == "linking failed");
    CHECK(c.diags.find("undefined symbol: f") != std::string::npos);

    for (const std::string_view arg : {"-Wl,--gc-sections", "-Wl,--defsym=x", "-Wl,--defsym==1"}) {
        c = compile_zig(main_zig, {arg});
        CHECK(!c.ok);
        CHECK(c.error.starts_with("unsupported linker argument '"));
    }

    c = compile_zig((dir.path / "missing.zig").string());
    CHECK(!c.ok);
    CHECK(c.error == "compilation failed");
}

TEST_CASE("toolchain: source kinds and exceptions") {
    CHECK(malm::is_c_source("a.c"));
    CHECK(!malm::is_c_source(".c"));
    CHECK(!malm::is_c_source("a.cpp"));
    for (const char* name : {"a.cpp", "dir/a.cc", "a.cxx"}) {
        CHECK(malm::is_cxx_source(name));
    }
    CHECK(!malm::is_cxx_source("a.c"));
    CHECK(!malm::is_cxx_source(".cpp"));
    CHECK(!malm::is_cxx_source("a.hpp"));
    CHECK(malm::is_rust_source("src/main.rs"));
    CHECK(!malm::is_rust_source(".rs"));
    CHECK(!malm::is_rust_source("a.rlib"));
    CHECK(malm::is_zig_source("src/main.zig"));
    CHECK(!malm::is_zig_source(".zig"));
    CHECK(!malm::is_zig_source("build.zig.zon"));
    using Args = std::vector<std::string_view>;
    CHECK(!malm::wants_exceptions(Args{}));
    CHECK(malm::wants_exceptions(Args{"-O1", "-fexceptions"}));
    CHECK(!malm::wants_exceptions(Args{"-fexceptions", "-fno-exceptions"}));
    CHECK(malm::wants_exceptions(Args{"-fno-exceptions", "-fexceptions", "-frtti"}));
}
