// The HLA examples of examples/hla (issue #19): hello and cat produce exactly the output of their
// assembly counterparts in examples/asm, and `halt 42` exits with 42 through EXIT_A.

#include <doctest/doctest.h>

#include <random>
#include <string>

#include "asm/check.hpp"
#include "hla_test_util.hpp"

using namespace hlatest;

namespace {

assembler::Assembly hla_example(const std::string& name) {
    assembler::FsLoader loader;
    assembler::Diagnostics diags;
    const assembler::Program p =
        hla::translate(std::string(MALM_HLA_EXAMPLES_DIR) + "/" + name, loader, diags);
    INFO(assembler::format_diagnostics(diags, p.files));
    REQUIRE(!diags.has_errors());
    auto a = assembler::assemble(p, diags);
    REQUIRE(a.has_value());
    return std::move(*a);
}

assembler::Assembly asm_example(const std::string& name) {
    assembler::FsLoader loader;
    assembler::Diagnostics diags;
    const assembler::Program p =
        assembler::analyze(std::string(MALM_ASM_EXAMPLES_DIR) + "/" + name, loader, diags);
    REQUIRE(!diags.has_errors());
    auto a = assembler::assemble(p, diags);
    REQUIRE(a.has_value());
    return std::move(*a);
}

} // namespace

TEST_CASE("hello.mhl prints what hello.mua prints") {
    const Ran h = run(hla_example("hello.mhl"));
    CHECK(h.out == "Hello, world!\n");
    CHECK(h.out == run(asm_example("hello.mua")).out);
    CHECK(h.status == 0);
}

TEST_CASE("exit42.mhl exits with status 42") { CHECK(run(hla_example("exit42.mhl")).status == 42); }

TEST_CASE("cat.mhl copies like halting_cat.mua, and like cat.mua up to EOF") {
    const assembler::Assembly hla_cat = hla_example("cat.mhl");
    const assembler::Assembly halting = asm_example("halting_cat.mua");
    const assembler::Assembly endless = asm_example("cat.mua");
    std::string all;
    for (int i = 0; i < 256; ++i) {
        all += static_cast<char>(i);
    }
    std::vector<std::string> inputs = {"", "Hello, cat!\n", all + all, std::string(1, '\xff')};
    std::mt19937 rng(19);
    for (int i = 0; i < 10; ++i) {
        std::string s;
        for (auto k = rng() % 40; k > 0; --k) {
            s += static_cast<char>(rng() % 256);
        }
        inputs.push_back(s);
    }
    for (const std::string& in : inputs) {
        CAPTURE(in);
        const Ran r = run(hla_cat, in);
        CHECK(r.out == in);
        CHECK(r.status == 0);
        CHECK(r.out == run(halting, in).out);
        // cat.mua never halts: after the input it writes byte 168 forever.
        malm::core::Vm vm(endless.startup.text);
        ScriptIo io;
        io.in = in;
        vm.run(io, endless.startup.steps + 1'000'000);
        REQUIRE(io.out.size() > in.size());
        CHECK(io.out.substr(0, in.size()) == r.out);
    }
}
