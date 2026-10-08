#pragma once

// Helpers shared by the layout tests: assemble a source up to the memory image, and load an image
// into a VM the way the startup code (#15) will leave it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asm/assemble.hpp"
#include "asm/layout.hpp"
#include "asm_test_util.hpp"
#include "core/vm.hpp"

namespace asmtest {

using malm::core::Value;

struct Laid {
    Parsed parsed;
    std::optional<Layout> layout;
};

inline Laid lay_out(std::string_view src, Files extra = {}) {
    Laid r;
    r.parsed = analyze_src(src, std::move(extra));
    if (!r.parsed.diags.has_errors()) {
        r.layout = layout_program(r.parsed.program, r.parsed.diags);
    }
    return r;
}

// A layout that must succeed.
inline Layout laid(std::string_view src) {
    Laid r = lay_out(src);
    INFO(r.parsed.text());
    REQUIRE(r.layout.has_value());
    REQUIRE(!r.parsed.diags.has_errors());
    return std::move(*r.layout);
}

// The errors of a layout that must fail (`L:C: message`).
inline std::vector<std::string> layout_errors(std::string_view src) {
    Laid r = lay_out(src);
    CHECK_FALSE(r.layout.has_value());
    return r.parsed.errors();
}

inline Value nat(std::uint64_t k) { return *Value::natural(k); }

inline Value addr_of(const Layout& l, const std::string& label) {
    const auto it = l.labels.find(label);
    REQUIRE(it != l.labels.end());
    return it->second.addr;
}

inline const ImageCell& cell_at(const Layout& l, Value addr) {
    const ImageCell* c = l.find(addr);
    REQUIRE(c != nullptr);
    return *c;
}

inline std::uint8_t char_at(const Layout& l, Value addr) {
    const ImageCell& c = cell_at(l, addr);
    REQUIRE(c.value.raw() >= 33);
    REQUIRE(c.value.raw() <= 126);
    return static_cast<std::uint8_t>(c.value.raw());
}

inline Value plus(Value v, std::int64_t k) { return *add(v, *Value::from_int(k)); }

// A whole assembly (layout and startup code) that must succeed.
inline Assembly assembled(std::string_view src) {
    Parsed p = analyze_src(src);
    INFO(p.text());
    REQUIRE(!p.diags.has_errors());
    auto a = assemble(p.program, p.diags);
    INFO(p.text());
    REQUIRE(a.has_value());
    return std::move(*a);
}

// Scripted input, recorded output.
struct TestIo final : malm::core::Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int read_byte() override {
        return pos < in.size() ? static_cast<unsigned char>(in[pos++]) : malm::core::kEof;
    }
    bool write_byte(std::uint8_t b) override {
        out += static_cast<char>(b);
        return true;
    }
    bool flush() override { return true; }
};

// The VM after the startup code has run (docs/asm.md 5.3): C at its final Jmp, D at ENTRY, n equal
// to the program width, every image cell written and no I/O done.
inline malm::core::Vm start(const Assembly& a) {
    malm::core::Vm vm(a.startup.text);
    TestIo io;
    const auto r = vm.run(io, a.startup.steps);
    if (r.fault) {
        INFO(malm::core::format_fault(*r.fault));
        REQUIRE(false);
    }
    REQUIRE(r.kind == malm::core::RunResult::Kind::StepLimit);
    CHECK(io.out.empty());
    CHECK(io.pos == 0);
    REQUIRE(vm.d() == addr_of(a.layout, "ENTRY"));
    REQUIRE(vm.n() == a.layout.width);
    REQUIRE(malm::core::decode(static_cast<std::uint32_t>(vm.c().mod94()),
                               static_cast<std::uint32_t>(vm.memory().read(vm.c()).raw())) ==
            malm::core::Op::Jmp);
    for (const ImageCell& c : a.layout.cells) {
        INFO(c.addr.to_string());
        REQUIRE(vm.memory().read(c.addr) == c.value);
    }
    return vm;
}

} // namespace asmtest
