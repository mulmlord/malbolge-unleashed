// The golden examples of examples/asm (issue #16): each one is assembled from its file, checked for
// purity (the text passes the strict loader; the .malm holds exactly that text) and run in
// unleashed mode on scripted input.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "core/loader.hpp"
#include "layout_test_util.hpp"

using namespace asmtest;

namespace {

Assembly assemble_example(const std::string& name) {
    FsLoader loader;
    Diagnostics diags;
    const Program program = analyze(std::string(MALM_EXAMPLES_DIR) + "/" + name, loader, diags);
    INFO(format_diagnostics(diags, program.files));
    REQUIRE(!diags.has_errors());
    auto a = assemble(program, diags);
    INFO(format_diagnostics(diags, program.files));
    REQUIRE(a.has_value());
    return std::move(*a);
}

// Runs the assembled program on `input` for at most `max_steps` steps.
std::string run(const Assembly& a, const std::string& input, bool halts = true,
                std::uint64_t max_steps = 50'000'000) {
    malm::core::Vm vm(a.startup.text);
    TestIo io;
    io.in = input;
    const auto r = vm.run(io, max_steps);
    if (r.fault) {
        FAIL(malm::core::format_fault(*r.fault));
    }
    CHECK(r.kind ==
          (halts ? malm::core::RunResult::Kind::Halted : malm::core::RunResult::Kind::StepLimit));
    return io.out;
}

struct Example {
    std::string file;
    Assembly a;
    explicit Example(const std::string& f) : file(f), a(assemble_example(f)) {}
};

} // namespace

TEST_CASE("examples are pure, deterministic and start as docs/asm.md 5.3 promises") {
    for (const char* f :
         {"hello.mua", "cat.mua", "halting_cat.mua", "digital_root.mua", "adder.mua"}) {
        CAPTURE(f);
        const Example e(f);
        const auto& text = e.a.startup.text;
        const auto loaded = malm::core::load_text(text, malm::core::kUnleashedMaxCells);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value() == text);
        const auto bytes = malm::container::write_malm({text});
        const auto back = malm::container::read_malm(bytes, {malm::core::kUnleashedMaxCells});
        REQUIRE(back.has_value());
        CHECK(back->cells == text);
        CHECK(assemble_example(f).startup.text == text);
        start(e.a);
    }
}

TEST_CASE("hello world") { CHECK(run(Example("hello.mua").a, "") == "Hello, world!\n"); }

TEST_CASE("cat echoes its input, then byte 168 forever") {
    const std::string out = run(Example("cat.mua").a, "abc", false, 2'000'000);
    REQUIRE(out.size() > 6);
    CHECK(out.substr(0, 3) == "abc");
    CHECK(out.find_first_not_of('\xa8', 3) == std::string::npos);
}

TEST_CASE("halting cat copies every byte value and halts on EOF") {
    const Example e("halting_cat.mua");
    std::string all;
    for (int i = 0; i < 256; ++i) {
        all += static_cast<char>(i);
    }
    CHECK(run(e.a, "") == "");
    CHECK(run(e.a, all + all) == all + all);
}

TEST_CASE("digital root") {
    const Example e("digital_root.mua");
    for (const auto& [in, want] :
         std::vector<std::pair<std::string, std::string>>{{"", "0"},
                                                          {"0", "0"},
                                                          {"000\n", "0"},
                                                          {"9", "9"},
                                                          {"18\n", "9"},
                                                          {"9875", "2"},
                                                          {"1234567890123", "6"},
                                                          {"0000500", "5"}}) {
        CAPTURE(in);
        CHECK(run(e.a, in) == want + "\n");
    }
    std::mt19937 rng(16);
    for (int i = 0; i < 20; ++i) {
        std::string in;
        unsigned sum = 0;
        for (auto k = rng() % 25; k > 0; --k) {
            const auto d = static_cast<unsigned>(rng() % 10);
            in += static_cast<char>('0' + d);
            sum += d;
        }
        CAPTURE(in);
        CHECK(run(e.a, in + "\n") == std::to_string(sum == 0 ? 0 : 1 + (sum - 1) % 9) + "\n");
    }
}

TEST_CASE("adder") {
    const Example e("adder.mua");
    for (const auto& [in, want] :
         std::vector<std::pair<std::string, std::string>>{{"123 456\n", "579"},
                                                          {"999 999\n", "1998"},
                                                          {"000 000", "0"},
                                                          {"001+009", "10"},
                                                          {"050,050", "100"},
                                                          {"099 901\n", "1000"},
                                                          {"007 000", "7"},
                                                          {"500 499", "999"}}) {
        CAPTURE(in);
        CHECK(run(e.a, in) == want + "\n");
    }
    std::mt19937 rng(16);
    for (int i = 0; i < 20; ++i) {
        const auto a = static_cast<unsigned>(rng() % 1000);
        const auto b = static_cast<unsigned>(rng() % 1000);
        char in[16];
        std::snprintf(in, sizeof in, "%03u %03u\n", a, b);
        CAPTURE(in);
        CHECK(run(e.a, in) == std::to_string(a + b) + "\n");
    }
    CHECK(run(e.a, "12") == ""); // EOF inside the numbers halts silently
}
