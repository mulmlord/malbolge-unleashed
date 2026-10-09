#pragma once

// Helpers shared by the HLA tests: compile a source held in memory, run it in the VM and read its
// registers and RAM back.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "asm/assemble.hpp"
#include "asm/parser.hpp"
#include "core/vm.hpp"
#include "hla/lower.hpp"
#include "hla/word.hpp"

namespace hlatest {

using namespace malm;

struct Compiled {
    assembler::Program program;
    assembler::Assembly assembly;
};

// The diagnostics of `src` (as file t.mhl), formatted; empty if it compiles.
inline std::string diagnostics(const std::string& src) {
    assembler::MemoryLoader loader({{"t.mhl", src}});
    assembler::Diagnostics diags;
    const assembler::Program p = hla::translate("t.mhl", loader, diags);
    return assembler::format_diagnostics(diags, p.files);
}

inline Compiled compile(const std::string& src) {
    assembler::MemoryLoader loader({{"t.mhl", src}});
    assembler::Diagnostics diags;
    assembler::Program p = hla::translate("t.mhl", loader, diags);
    INFO(assembler::format_diagnostics(diags, p.files));
    REQUIRE(!diags.has_errors());
    auto a = assembler::assemble(p, diags);
    INFO(assembler::format_diagnostics(diags, p.files));
    REQUIRE(a.has_value());
    return {std::move(p), std::move(*a)};
}

struct ScriptIo final : core::Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int read_byte() override {
        return pos < in.size() ? static_cast<unsigned char>(in[pos++]) : core::kEof;
    }
    bool write_byte(std::uint8_t b) override {
        out += static_cast<char>(b);
        return true;
    }
    bool flush() override { return true; }
};

struct Ran {
    std::string out;
    int status = -1; // the exit status with EXIT_A, -1 if the program did not halt
    std::optional<core::Vm> vm;
};

inline Ran run(const assembler::Assembly& a, const std::string& input = {},
               std::uint64_t max_steps = 200'000'000) {
    Ran r;
    r.vm.emplace(a.startup.text);
    ScriptIo io;
    io.in = input;
    const auto res = r.vm->run(io, max_steps);
    if (res.fault) {
        FAIL(core::format_fault(*res.fault));
    }
    r.out = io.out;
    if (res.kind == core::RunResult::Kind::Halted) {
        r.status = core::exit_status(res, *r.vm, true);
    }
    return r;
}

// The word in register `name` after the run.
inline std::uint32_t reg(const Compiled& c, const Ran& r, const std::string& name) {
    const auto it = c.assembly.layout.labels.find("V_" + name);
    REQUIRE(it != c.assembly.layout.labels.end());
    const auto w = hla::word_value(r.vm->memory().read(it->second.addr));
    REQUIRE(w.has_value());
    return *w;
}

// The RAM word at byte address `addr` after the run.
inline std::uint32_t ram_word(const Ran& r, std::uint32_t addr) {
    const auto w = hla::word_value(r.vm->memory().read(hla::ram_record(addr)));
    REQUIRE(w.has_value());
    return *w;
}

} // namespace hlatest
