#include "rv/semantics.hpp"

#include <array>
#include <bit>
#include <format>

namespace malm::rv::sem {
namespace {

std::string num(std::uint32_t v) { return std::format("0x{:X}", v); }

// The 256 labels of a switch on a byte address's low byte: by the address mod 4.
std::vector<std::string> by_lane(const std::array<std::string, 4>& lanes) {
    std::vector<std::string> labels;
    for (unsigned b = 0; b < 256; ++b) {
        labels.push_back(lanes[b % 4]);
    }
    return labels;
}

std::string lane_label(std::string_view sub, unsigned k) { return std::format("{}{}", sub, k); }

// A subroutine of `width` bytes: checks ea, then dispatches on ea mod 4 to the labels
// <name>0 .. <name>3. With `word0`, w0 holds the aligned word at ea first.
void lane_dispatch(Writer& w, const Config& c, std::string_view name, std::string_view fault,
                   bool word0) {
    w.label(name);
    check_in_ram(w, c, "ea", fault);
    w.label(std::format("{}_in", name)); // entry without the check (a range checked before)
    if (word0) {
        w.stmt("and wa, ea, -4");
        w.stmt("load w0, [wa]");
    }
    w.switch_on("ea", by_lane({lane_label(name, 0), lane_label(name, 1), lane_label(name, 2),
                               lane_label(name, 3)}));
}

// The next word of an access that crosses a word boundary: wa := its address (ea rounded up to a
// multiple of 4, from ea mod 4 = k), checked against the end of guest RAM.
void next_word(Writer& w, const Config& c, unsigned k, std::string_view fault) {
    w.stmt(std::format("add wa, ea, {}", 4 - k));
    check_in_ram(w, c, "wa", fault);
}

void loads(Writer& w, const Config& c) {
    w.comment("ld_b, ld_h, ld_w: r := the byte, halfword or word at ea (zero-extended)");
    lane_dispatch(w, c, "ld_b", kFaultLoad, true);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("ld_b", k));
        if (k == 0) {
            w.stmt("and r, w0, 0xFF");
        } else {
            w.stmt(std::format("srl r, w0, {}", 8 * k));
            if (k < 3) {
                w.stmt("and r, r, 0xFF");
            }
        }
        w.stmt("jmp lkm");
    }

    lane_dispatch(w, c, "ld_h", kFaultLoad, true);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("ld_h", k));
        if (k == 0) {
            w.stmt("and r, w0, 0xFFFF");
        } else if (k < 3) {
            w.stmt(std::format("srl r, w0, {}", 8 * k));
            if (k == 1) {
                w.stmt("and r, r, 0xFFFF");
            }
        } else { // the high byte is the next word's first
            next_word(w, c, k, kFaultLoad);
            w.stmt("load w1, [wa]");
            w.stmt("srl r, w0, 24");
            w.stmt("and w1, w1, 0xFF");
            w.stmt("sll w1, w1, 8");
            w.stmt("or r, r, w1");
        }
        w.stmt("jmp lkm");
    }

    lane_dispatch(w, c, "ld_w", kFaultLoad, false);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("ld_w", k));
        if (k == 0) {
            w.stmt("load r, [ea]");
        } else { // 4 - k bytes from this word, k from the next
            next_word(w, c, k, kFaultLoad);
            w.stmt("load w1, [wa]");
            w.stmt("and wa, ea, -4");
            w.stmt("load w0, [wa]");
            w.stmt(std::format("srl w0, w0, {}", 8 * k));
            w.stmt(std::format("sll w1, w1, {}", 32 - 8 * k));
            w.stmt("or r, w0, w1");
        }
        w.stmt("jmp lkm");
    }
}

// w0 := w0 with the bytes of `mask` (in place) replaced by those of u.
void merge(Writer& w, std::string_view word, std::uint32_t mask) {
    w.stmt(std::format("and {0}, {0}, {1}", word, num(~mask)));
    w.stmt(std::format("or {0}, {0}, u", word));
}

void stores(Writer& w, const Config& c) {
    w.comment("st_b, st_h, st_w: the byte, halfword or word at ea := the low bytes of b");
    lane_dispatch(w, c, "st_b", kFaultStore, true);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("st_b", k));
        if (k < 3) {
            w.stmt("and u, b, 0xFF");
            if (k > 0) {
                w.stmt(std::format("sll u, u, {}", 8 * k));
            }
        } else {
            w.stmt("sll u, b, 24");
        }
        merge(w, "w0", 0xFFu << (8 * k));
        w.stmt("store [wa], w0");
        w.stmt("jmp lkm");
    }

    lane_dispatch(w, c, "st_h", kFaultStore, true);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("st_h", k));
        if (k == 3) { // the low byte goes to this word, the high byte to the next
            next_word(w, c, k, kFaultStore);
            w.stmt("load w1, [wa]");
            w.stmt("srl u, b, 8");
            w.stmt("and u, u, 0xFF");
            merge(w, "w1", 0xFF);
            w.stmt("store [wa], w1");
            w.stmt("and wa, ea, -4");
            w.stmt("sll u, b, 24");
            merge(w, "w0", 0xFF000000);
        } else {
            if (k < 2) {
                w.stmt("and u, b, 0xFFFF");
                if (k == 1) {
                    w.stmt("sll u, u, 8");
                }
            } else {
                w.stmt("sll u, b, 16");
            }
            merge(w, "w0", 0xFFFFu << (8 * k));
        }
        w.stmt("store [wa], w0");
        w.stmt("jmp lkm");
    }

    lane_dispatch(w, c, "st_w", kFaultStore, false);
    for (unsigned k = 0; k < 4; ++k) {
        w.label(lane_label("st_w", k));
        if (k == 0) {
            w.stmt("store [ea], b");
        } else { // the low 4 - k bytes to this word, the other k to the next
            const std::uint32_t low = (1u << (8 * k)) - 1; // the bytes of this word kept
            next_word(w, c, k, kFaultStore);
            w.stmt("load w1, [wa]");
            w.stmt(std::format("srl u, b, {}", 32 - 8 * k));
            merge(w, "w1", low);
            w.stmt("store [wa], w1");
            w.stmt("and wa, ea, -4");
            w.stmt("load w0, [wa]");
            w.stmt(std::format("sll u, b, {}", 8 * k));
            merge(w, "w0", ~low);
            w.stmt("store [wa], w0");
        }
        w.stmt("jmp lkm");
    }
}

// a0 := -errno, return.
void sys_error(Writer& w, std::string_view label, std::int32_t err) {
    w.label(label);
    w.stmt(std::format("mov x10, {}", err));
    w.stmt("jmp lke");
}

void system_calls(Writer& w, const Config& c) {
    w.comment("ecall: the system call a7 (docs/rv.md 4)");
    w.label("ecall");
    std::vector<std::string> by_number(215, "sys_nosys");
    by_number[63] = "sys_read";
    by_number[64] = "sys_write";
    by_number[93] = "sys_exit";
    by_number[214] = "sys_brk";
    w.switch_on("x17", by_number);
    w.stmt("jmp sys_nosys");
    const auto high_zero = [&](std::string_view reg, std::string_view otherwise) {
        w.stmt(std::format("srl t, {}, 8", reg)); // the switch saw the low byte only
        w.stmt(std::format("bnz t, {}", otherwise));
    };
    // The buffer [a1, a1 + a2) inside guest RAM: a2 <= ram_size and a1 <= ram_size - a2.
    const auto buffer_in_ram = [&] {
        w.stmt(std::format("sltu t, x12, {}", num(c.ram_size + 1)));
        w.stmt("bz t, sys_efault");
        w.stmt(std::format("mov u, {}", num(c.ram_size)));
        w.stmt("sub u, u, x12");
        w.stmt("sltu t, u, x11");
        w.stmt("bnz t, sys_efault");
        w.stmt("mov ea, x11");
        w.stmt("mov n, x12");
    };

    // read(0, buf, count): one In per byte until count bytes or end of input.
    w.label("sys_read");
    high_zero("x17", "sys_nosys");
    w.stmt("bnz x10, sys_ebadf");
    buffer_in_ram();
    w.label("sys_read_loop");
    w.stmt("bz n, sys_read_end");
    w.stmt("in b, sys_read_end");
    w.call("lkm", "st_b_in", "sys_read_next");
    w.stmt("add ea, ea, 1");
    w.stmt("sub n, n, 1");
    w.stmt("jmp sys_read_loop");
    w.label("sys_read_end");
    w.stmt("sub x10, x12, n");
    w.stmt("jmp lke");

    // write(1 or 2, buf, count)
    w.label("sys_write");
    high_zero("x17", "sys_nosys");
    w.switch_on("x10", {"sys_ebadf", "sys_write_fd", "sys_write_fd"});
    w.stmt("jmp sys_ebadf");
    w.label("sys_write_fd");
    high_zero("x10", "sys_ebadf");
    buffer_in_ram();
    w.label("sys_write_loop");
    w.stmt("bz n, sys_write_end");
    w.call("lkm", "ld_b_in", "sys_write_next");
    w.stmt("out r");
    w.stmt("add ea, ea, 1");
    w.stmt("sub n, n, 1");
    w.stmt("jmp sys_write_loop");
    w.label("sys_write_end");
    w.stmt("mov x10, x12");
    w.stmt("jmp lke");

    w.label("sys_exit");
    high_zero("x17", "sys_nosys");
    w.stmt(std::format("mov stop, {}", static_cast<std::uint32_t>(StopCode::Exit)));
    w.stmt(c.harness ? "jmp fetch" : "halt x10");

    // brk(addr): the break := addr if brkmin <= addr <= ram_size; a0 := the break.
    w.label("sys_brk");
    high_zero("x17", "sys_nosys");
    w.stmt("sltu t, x10, brkmin");
    w.stmt("bnz t, sys_brk_end");
    w.stmt(std::format("sltu t, x10, {}", num(c.ram_size + 1)));
    w.stmt("bz t, sys_brk_end");
    w.stmt("mov brk, x10");
    w.label("sys_brk_end");
    w.stmt("mov x10, brk");
    w.stmt("jmp lke");

    sys_error(w, "sys_nosys", -38);
    sys_error(w, "sys_ebadf", -9);
    sys_error(w, "sys_efault", -14);
}

} // namespace

void Writer::stmt(std::string_view s) {
    text_ += "        ";
    text_ += s;
    text_ += '\n';
}

void Writer::label(std::string_view name) {
    text_ += name;
    text_ += ":\n";
}

void Writer::comment(std::string_view text) {
    text_ += "; ";
    text_ += text;
    text_ += '\n';
}

void Writer::blank() { text_ += '\n'; }

void Writer::switch_on(std::string_view reg, const std::vector<std::string>& labels) {
    std::string s = std::format("switch {}", reg);
    for (const std::string& l : labels) {
        s += ", ";
        s += l;
    }
    stmt(s);
}

std::string Writer::fresh(std::string_view prefix) { return std::format("{}_{}", prefix, ++next_); }

void Writer::call(std::string_view link, std::string_view target, std::string_view ret) {
    stmt(std::format("mov {}, {}", link, ret));
    stmt(std::format("jmp {}", target));
    label(ret);
}

void declare_ram(Writer& w, std::uint32_t ram_size, const Image* image) {
    std::vector<std::uint32_t> words;
    if (image != nullptr) {
        for (const Segment& s : image->segments) {
            for (std::size_t i = 0; i < s.data.size(); ++i) {
                const std::size_t addr = s.vaddr + i;
                if (words.size() <= addr / 4) {
                    words.resize(addr / 4 + 1);
                }
                words[addr / 4] |= std::uint32_t{s.data[i]} << (8 * (addr % 4));
            }
        }
        while (!words.empty() && words.back() == 0) {
            words.pop_back();
        }
    }
    std::string ram = std::format("ram mem[{}]", ram_size / 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        ram += i == 0 ? " = " : ", ";
        ram += num(words[i]);
    }
    w.stmt(ram);
}

void declare_runtime(Writer& w, std::uint32_t brk) {
    w.stmt("reg ea, b, r, lkm, lke, t, u, n, wa, w0, w1");
    w.stmt(std::format("reg brk = {0}, brkmin = {0}, stop", num(brk)));
}

void check_in_ram(Writer& w, const Config& c, std::string_view reg, std::string_view fault) {
    if (std::has_single_bit(c.ram_size)) { // no bit at or above ram_size's
        w.stmt(std::format("and t, {}, {}", reg, num(~(c.ram_size - 1))));
        w.stmt(std::format("bnz t, {}", fault));
    } else {
        w.stmt(std::format("sltu t, {}, {}", reg, num(c.ram_size)));
        w.stmt(std::format("bz t, {}", fault));
    }
}

void alu(Writer& w, Alu op, std::string_view d, std::string_view x, std::string_view y) {
    static constexpr std::array<std::string_view, 18> kMnemonic = {
        "add", "sub", "sll",  "slt",    "sltu",  "xor", "srl",  "sra", "or",
        "and", "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
    w.stmt(std::format("{} {}, {}, {}", kMnemonic[static_cast<std::size_t>(op)], d, x, y));
}

void branch(Writer& w, Cond c, std::string_view x, std::string_view y, std::string_view taken) {
    switch (c) {
    case Cond::Eq:
    case Cond::Ne:
        w.stmt(std::format("xor t, {}, {}", x, y));
        break;
    case Cond::Lt:
    case Cond::Ge:
        w.stmt(std::format("slt t, {}, {}", x, y));
        break;
    case Cond::Ltu:
    case Cond::Geu:
        w.stmt(std::format("sltu t, {}, {}", x, y));
        break;
    }
    // t is 0 for Eq (x ^ y = 0) and for Ge, Geu (not less).
    const bool zero = c == Cond::Eq || c == Cond::Ge || c == Cond::Geu;
    w.stmt(std::format("{} t, {}", zero ? "bz" : "bnz", taken));
}

void load(Writer& w, Width width, bool sign, std::string_view ret) {
    static constexpr std::array<std::string_view, 5> kSub = {"", "ld_b", "ld_h", "", "ld_w"};
    const std::string_view sub = kSub[static_cast<std::size_t>(width)];
    if (!sign || width == Width::Word) {
        w.stmt(std::format("mov lkm, {}", ret));
        w.stmt(std::format("jmp {}", sub));
        return;
    }
    w.call("lkm", sub, w.fresh(sub));
    w.stmt(std::format("{} r, r", width == Width::Byte ? "sextb" : "sexth"));
    w.stmt(std::format("jmp {}", ret));
}

void store(Writer& w, Width width, std::string_view ret) {
    static constexpr std::array<std::string_view, 5> kSub = {"", "st_b", "st_h", "", "st_w"};
    w.stmt(std::format("mov lkm, {}", ret));
    w.stmt(std::format("jmp {}", kSub[static_cast<std::size_t>(width)]));
}

void ecall(Writer& w, std::string_view ret) {
    w.stmt(std::format("mov lke, {}", ret));
    w.stmt("jmp ecall");
}

void runtime(Writer& w, const Config& c) {
    loads(w, c);
    w.blank();
    stores(w, c);
    w.blank();
    system_calls(w, c);
    w.blank();
    w.comment("stops: `stop` says why; pc names the instruction");
    const auto stop = [&](std::string_view label, StopCode code) {
        w.label(label);
        w.stmt(std::format("mov stop, {}", static_cast<std::uint32_t>(code)));
        w.stmt("jmp stopped");
    };
    stop(kFaultIllegal, StopCode::Illegal);
    stop(kFaultEbreak, StopCode::Ebreak);
    stop(kFaultMisaligned, StopCode::MisalignedJump);
    stop(kFaultLoad, StopCode::LoadFault);
    stop(kFaultStore, StopCode::StoreFault);
    stop(kFaultFetch, StopCode::FetchFault);
    w.label("stopped");
    w.stmt(c.harness ? "jmp fetch" : "halt 1");
}

} // namespace malm::rv::sem
