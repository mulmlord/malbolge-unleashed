// Calls, the stack, switch and computed jumps (issue #20): compiled, assembled and run in the VM.
// The acceptance programs: a recursive factorial and a jump-table dispatcher checked against a C++
// model of it.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "hla_test_util.hpp"

using namespace hlatest;

TEST_CASE("call and ret: nested calls, several call sites, a computed call") {
    const Compiled c = compile("reg f = b\n"
                               "        call a\n"
                               "        out '.'\n"
                               "        call b\n"
                               "        out '.'\n"
                               "        call f\n"
                               "        halt 9\n"
                               "a:      out 'a'\n"
                               "        call b\n"
                               "        out 'A'\n"
                               "        ret\n"
                               "b:      out 'b'\n"
                               "        ret\n");
    const Ran r = run(c.assembly);
    CHECK(r.out == "abA.b.b");
    CHECK(r.status == 9);
}

TEST_CASE("push and pop: last in, first out; registers, numbers, labels and RAM regions") {
    const Compiled c = compile("reg a = 0x12345678, b, x, y, z, t\n"
                               "ram m[2]\n"
                               "        push a\n"
                               "        push -1\n"
                               "        push m\n"
                               "        push l\n"
                               "        pop t\n"
                               "        pop x\n"
                               "        pop y\n"
                               "        pop z\n"
                               "        push z\n"
                               "        pop b\n"
                               "        jmp t\n"
                               "        halt 1\n"
                               "l:      halt 2\n");
    const Ran r = run(c.assembly);
    CHECK(r.status == 2);
    CHECK(reg(c, r, "x") == 0); // m's base address
    CHECK(reg(c, r, "y") == 0xFFFFFFFF);
    CHECK(reg(c, r, "z") == 0x12345678);
    CHECK(reg(c, r, "b") == 0x12345678);
    CHECK(reg(c, r, "a") == 0x12345678);
}

TEST_CASE("the stack holds its declared size; overflow and underflow halt with 255") {
    std::string src = "stack 5\nreg a\n";
    for (int i = 0; i < 5; ++i) {
        src += "        push " + std::to_string(i) + "\n";
    }
    src += "        out 'f'\n";
    for (int i = 0; i < 5; ++i) {
        src += "        pop a\n        out a\n";
    }
    src += "        bz a, under\n        halt 1\n";
    src += "under:  pop a\n        halt 2\n";
    SUBCASE("underflow") {
        const Ran r = run(compile(src).assembly);
        CHECK(r.out == std::string("f\4\3\2\1\0", 6));
        CHECK(r.status == 255);
    }
    SUBCASE("overflow") {
        const Ran r =
            run(compile("stack 2\n push 1\n push 2\n out 'k'\n call f\nf: halt 3\n").assembly);
        CHECK(r.out == "k");
        CHECK(r.status == 255);
    }
}

TEST_CASE("switch goes to the label of the low byte, or on past the labels") {
    std::string src = "reg x\n";
    std::vector<std::uint32_t> values = {0, 1, 2, 3, 4, 255, 0x100, 0x102, 0xFFFFFF01, 7};
    std::string want;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string k = std::to_string(i);
        src += "        mov x, " + std::to_string(values[i]) + "\n";
        src += "        switch x, a" + k + ", b" + k + ", c" + k + "\n";
        src += "        out 'n'\n        jmp e" + k + "\n";
        src += "a" + k + ":  out 'a'\n        jmp e" + k + "\n";
        src += "b" + k + ":  out 'b'\n        jmp e" + k + "\n";
        src += "c" + k + ":  out 'c'\n";
        src += "e" + k + ":\n";
        const std::uint32_t b = values[i] & 0xFF;
        want += b < 3 ? static_cast<char>('a' + b) : 'n';
    }
    // 256 labels: every byte has one.
    src += "        mov x, 0x4241\n        switch x";
    for (int b = 0; b < 256; ++b) {
        src += b == 0x41 ? ", hit" : ", miss";
    }
    src += "\nmiss:   halt 1\nhit:    halt 2\n";
    const Compiled c = compile(src);
    const Ran r = run(c.assembly);
    CHECK(r.out == want);
    CHECK(r.status == 2);
    CHECK(reg(c, r, "x") == 0x4241); // switch keeps the register
}

namespace {

// fact(n) by recursion, for n = 0..5. Without arithmetic (#21), n - 1 and acc + 1 are switches over
// the values they meet: `dec` and `inc` map x to x - 1 and x + 1. fact multiplies by repeated
// increments.
std::string factorial_program() {
    std::string s = "reg n, r, x, k, t, acc\n"
                    "stack 32\n";
    for (int n = 0; n <= 5; ++n) {
        s += "        mov n, " + std::to_string(n) + "\n        call fact\n        out r\n";
    }
    s += "        halt r\n"
         "; r := n!, n unchanged\n"
         "fact:   bnz n, rec\n"
         "        mov r, 1\n"
         "        ret\n"
         "rec:    push n\n"
         "        mov x, n\n"
         "        call dec\n"
         "        mov n, x\n"
         "        call fact\n"
         "        pop n\n"
         "; r := n * r: acc := 0, then n times r increments\n"
         "        mov acc, 0\n"
         "        mov k, n\n"
         "outer:  bz k, product\n"
         "        mov t, r\n"
         "inner:  bz t, next\n"
         "        mov x, acc\n"
         "        call inc\n"
         "        mov acc, x\n"
         "        mov x, t\n"
         "        call dec\n"
         "        mov t, x\n"
         "        jmp inner\n"
         "next:   mov x, k\n"
         "        call dec\n"
         "        mov k, x\n"
         "        jmp outer\n"
         "product: mov r, acc\n"
         "        ret\n";
    const auto table = [&](const std::string& name, int from, int to, int delta) {
        s += name + ":  switch x";
        for (int v = 0; v <= to; ++v) {
            s += ", " + (v < from ? std::string("bad") : name + std::to_string(v));
        }
        s += "\n        jmp bad\n";
        for (int v = from; v <= to; ++v) {
            s += name + std::to_string(v) + ": mov x, " + std::to_string(v + delta) + "\n";
            s += "        ret\n";
        }
    };
    table("dec", 1, 24, -1);
    table("inc", 0, 119, 1);
    s += "bad:    halt 99\n";
    return s;
}

} // namespace

TEST_CASE("recursive factorial") {
    const Compiled c = compile(factorial_program());
    const Ran r = run(c.assembly);
    CHECK(r.out == std::string("\1\1\2\6\x18\x78", 6));
    CHECK(r.status == 120);
    CHECK(reg(c, r, "n") == 5);
}

namespace {

// A command interpreter: a switch on the command byte, a stack, and calls through a jump table in
// RAM. Commands: `p<b>` push the byte b, `o` pop and write it, `d` duplicate, `s` swap the top two,
// `j<i>` call handler i (i = 0, 4 or 8: a byte address in the table), `q` halt with the popped
// byte; any other byte writes '?'. EOF halts with 0.
std::string dispatcher_program() {
    // The switch's labels: every byte below 's' + 1 has one (the unknown ones `other`).
    std::string s = "reg c, a, b, h\n"
                    "ram handlers[3] = h0, h1, h2\n"
                    "loop:   in c, eof\n"
                    "        switch c";
    const std::string cmds = "podsjq";
    for (int b = 0; b <= 's'; ++b) {
        const auto pos = cmds.find(static_cast<char>(b));
        s += ", " + (pos == std::string::npos ? std::string("other")
                                              : std::string("cmd_") + static_cast<char>(b));
    }
    s += "\nother:  out '?'\n"
         "        jmp loop\n"
         "cmd_p:  in a\n"
         "        push a\n"
         "        jmp loop\n"
         "cmd_o:  pop a\n"
         "        out a\n"
         "        jmp loop\n"
         "cmd_d:  pop a\n"
         "        push a\n"
         "        push a\n"
         "        jmp loop\n"
         "cmd_s:  pop a\n"
         "        pop b\n"
         "        push a\n"
         "        push b\n"
         "        jmp loop\n"
         "cmd_j:  in a\n"
         "        load h, [a]\n"
         "        call h\n"
         "        jmp loop\n"
         "cmd_q:  pop a\n"
         "        halt a\n"
         "eof:    halt\n"
         "h0:     out \"<0>\"\n"
         "        ret\n"
         "h1:     out \"<1>\"\n"
         "        call h2\n"
         "        ret\n"
         "h2:     out \"<2>\"\n"
         "        ret\n";
    return s;
}

struct Model {
    std::string out;
    int status = 0;
};

Model run_model(const std::string& in) {
    Model m;
    std::vector<unsigned char> st;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == 'p') {
            st.push_back(static_cast<unsigned char>(in[++i]));
        } else if (c == 'o') {
            m.out += static_cast<char>(st.back());
            st.pop_back();
        } else if (c == 'd') {
            st.push_back(st.back());
        } else if (c == 's') {
            std::swap(st[st.size() - 1], st[st.size() - 2]);
        } else if (c == 'j') {
            const char h = in[++i];
            m.out += h == 0 ? "<0>" : h == 4 ? "<1><2>" : "<2>";
        } else if (c == 'q') {
            m.status = st.back();
            return m;
        } else {
            m.out += '?';
        }
    }
    return m;
}

} // namespace

TEST_CASE("a jump-table dispatcher matches its C++ model") {
    const Compiled c = compile(dispatcher_program());
    std::mt19937 rng(20);
    std::vector<std::string> inputs = {"",
                                       "pAo",
                                       "pApBsoo",
                                       "pxdoo",
                                       std::string("j\0j\4j\b", 6),
                                       "zz?",
                                       std::string("p\xffq", 3)};
    for (int i = 0; i < 6; ++i) {
        std::string in;
        std::size_t depth = 0;
        for (int k = 0; k < 25; ++k) {
            const std::string choices = depth >= 2 ? "podsjx" : depth == 1 ? "podjx" : "pjx";
            const char cmd = choices[rng() % choices.size()];
            in += cmd;
            if (cmd == 'p') {
                in += static_cast<char>(rng() % 256);
                ++depth;
            } else if (cmd == 'o') {
                --depth;
            } else if (cmd == 'd') {
                ++depth;
            } else if (cmd == 'j') {
                in += static_cast<char>(4 * (rng() % 3));
            } else if (cmd == 'x') {
                in.back() = static_cast<char>("abc\n\x80\xff!"[rng() % 7]);
            }
        }
        if (depth > 0 && rng() % 2) {
            in += 'q';
        }
        inputs.push_back(in);
    }
    for (const std::string& in : inputs) {
        CAPTURE(in);
        const Model m = run_model(in);
        const Ran r = run(c.assembly, in);
        CHECK(r.out == m.out);
        CHECK(r.status == m.status);
    }
}
