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

// The VM as the startup code leaves it (docs/asm.md 5.3): every image cell written, n equal to the
// program width, D at ENTRY and C at a Jmp (cell 0, `b`). Image cells that are characters go into
// the program text, the rest is written with poke. The text is long enough to make the initial n
// the program width: its last address has width n - 1.
inline malm::core::Vm load_image(const Layout& l) {
    using malm::core::pow3;
    std::uint64_t len = pow3[l.width - 2] + 1;
    for (const ImageCell& c : l.cells) {
        if (c.addr.head() == 0) {
            len = std::max(len, c.addr.raw() + 1);
        }
    }
    std::vector<std::uint8_t> text(len, '!');
    text[0] = 'b';
    std::vector<const ImageCell*> rest;
    for (const ImageCell& c : l.cells) {
        const bool is_char = c.value.raw() >= 33 && c.value.raw() <= 126;
        if (c.addr.head() == 0 && is_char) {
            text[c.addr.raw()] = static_cast<std::uint8_t>(c.value.raw());
        } else {
            rest.push_back(&c);
        }
    }
    malm::core::Vm vm(text);
    for (const ImageCell* c : rest) {
        vm.poke(c->addr, c->value);
    }
    vm.set_registers({Value(), Value(), addr_of(l, "ENTRY")});
    REQUIRE(vm.n() == l.width);
    return vm;
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

} // namespace asmtest
