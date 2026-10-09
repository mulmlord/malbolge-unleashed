// RAM (issue #20): random sequences of load and store, through address registers and constant
// addresses, against a C++ model of the memory; the words and registers are read back from the VM.

#include <doctest/doctest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "hla_test_util.hpp"

using namespace hlatest;

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

// A program of `ops` random statements on two regions, and the model's memory and registers.
struct Case {
    std::string src;
    std::map<std::uint32_t, std::uint32_t> mem; // byte address -> word
    std::map<std::string, std::uint32_t> regs;
};

Case random_case(std::uint32_t seed, int ops) {
    std::mt19937 rng(seed); // 32-bit draws
    Case c;
    const std::uint32_t m_words = 12;
    const std::uint32_t q_words = 5;
    const std::uint32_t q_base = 4 * m_words;
    const auto word = [&] {
        switch (rng() % 4) {
        case 0:
            return static_cast<std::uint32_t>(rng() % 4);
        case 1:
            return static_cast<std::uint32_t>(0xFFFFFFFFU - rng() % 2);
        default:
            return static_cast<std::uint32_t>(rng());
        }
    };
    const auto addr = [&]() -> std::uint32_t {
        return static_cast<std::uint32_t>(rng() % 3 ? 4 * (rng() % m_words)
                                                    : q_base + 4 * (rng() % q_words));
    };
    c.src = "reg a, v, r0, r1, r2, r3\nram m[" + std::to_string(m_words) + "] =";
    for (std::uint32_t i = 0; i < m_words; ++i) {
        const std::uint32_t w = i < 7 ? word() : 0;
        c.mem[4 * i] = w;
        if (i < 7) {
            c.src += std::string(i ? ", " : " ") + hex(w);
        }
    }
    c.src += "\nram q[" + std::to_string(q_words) + "]\n";
    for (std::uint32_t i = 0; i < q_words; ++i) {
        c.mem[q_base + 4 * i] = 0;
    }
    for (const char* r : {"a", "v", "r0", "r1", "r2", "r3"}) {
        c.regs[r] = 0;
    }
    const auto line = [&](const std::string& s) { c.src += "        " + s + "\n"; };
    const auto rr = [&] { return "r" + std::to_string(rng() % 4); };
    for (int i = 0; i < ops; ++i) {
        switch (rng() % 6) {
        case 0: { // store a register through an address register
            const std::uint32_t at = addr();
            const std::string src = rng() % 2 ? "v" : rr();
            if (src == "v") {
                c.regs["v"] = word();
                line("mov v, " + hex(c.regs["v"]));
            }
            c.regs["a"] = at;
            line("mov a, " + std::to_string(at));
            line("store [a], " + src);
            c.mem[at] = c.regs[src];
            break;
        }
        case 1: { // store a constant at a constant address
            const std::uint32_t at = addr();
            const std::uint32_t w = word();
            line("store [" + std::to_string(at) + "], " + hex(w));
            c.mem[at] = w;
            break;
        }
        case 2: { // load through an address register
            const std::uint32_t at = addr();
            const std::string dst = rr();
            c.regs["a"] = at;
            line("mov a, " + std::to_string(at));
            line("load " + dst + ", [a]");
            c.regs[dst] = c.mem[at];
            break;
        }
        case 3: { // load from a constant address, or a region's base
            const std::string dst = rr();
            if (rng() % 4 == 0) {
                line("load " + dst + ", [q]");
                c.regs[dst] = c.mem[q_base];
            } else {
                const std::uint32_t at = addr();
                line("load " + dst + ", [" + std::to_string(at) + "]");
                c.regs[dst] = c.mem[at];
            }
            break;
        }
        case 4: { // a pointer in RAM: store an address, load it, load through it
            const std::uint32_t slot = addr();
            const std::uint32_t at = addr();
            const std::string dst = rr();
            line("store [" + std::to_string(slot) + "], " + std::to_string(at));
            c.mem[slot] = at;
            line("load a, [" + std::to_string(slot) + "]");
            c.regs["a"] = at;
            line("load " + dst + ", [a]");
            c.regs[dst] = c.mem[at];
            break;
        }
        default: { // the address register itself as the value, and load into it
            const std::uint32_t at = addr();
            c.regs["a"] = at;
            line("mov a, " + std::to_string(at));
            if (rng() % 2) {
                line("store [a], a");
                c.mem[at] = at;
            } else {
                line("load a, [a]");
                c.regs["a"] = c.mem[at];
            }
            break;
        }
        }
    }
    line("halt");
    return c;
}

} // namespace

TEST_CASE("random load/store sequences match the C++ model") {
    for (const std::uint32_t seed : {1U, 2U, 3U}) {
        CAPTURE(seed);
        const Case k = random_case(seed, 70);
        INFO(k.src);
        const Compiled c = compile(k.src);
        const Ran r = run(c.assembly);
        CHECK(r.status == 0);
        for (const auto& [at, w] : k.mem) {
            CAPTURE(at);
            CHECK(ram_word(r, at) == w);
        }
        for (const auto& [name, w] : k.regs) {
            CAPTURE(name);
            CHECK(reg(c, r, name) == w);
        }
    }
}

TEST_CASE("RAM starts at its declared values, regions follow each other") {
    const Compiled c = compile("reg x, y, z\n"
                               "ram a[2] = 7\n"
                               "ram b[3] = 'b', l, a\n"
                               "        mov x, b\n"
                               "        load y, [x]\n"
                               "        load z, [16]\n"
                               "l:      halt\n");
    const Ran r = run(c.assembly);
    CHECK(reg(c, r, "x") == 8);
    CHECK(reg(c, r, "y") == 'b');
    CHECK(reg(c, r, "z") == 0);
    CHECK(ram_word(r, 0) == 7);
    CHECK(ram_word(r, 4) == 0);
    CHECK(ram_word(r, 8) == 'b');
    CHECK(hla::word_value(r.vm->memory().read(hla::ram_record(12))) == std::nullopt); // a label
}
