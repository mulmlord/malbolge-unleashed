#include "rv/machine.hpp"

#include <algorithm>
#include <cstdio>

namespace malm::rv {
namespace {

std::int32_t sx(std::uint32_t v) noexcept { return static_cast<std::int32_t>(v); }
std::uint32_t ux(std::int64_t v) noexcept { return static_cast<std::uint32_t>(v); }

std::string hex(std::uint32_t v) {
    char buf[11];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

Stop fault(std::string message) { return {Stop::Kind::Fault, 0, std::move(message)}; }

// Immediates of the base instruction formats, sign-extended.
std::uint32_t imm_i(std::uint32_t insn) noexcept { return ux(sx(insn) >> 20); }
std::uint32_t imm_s(std::uint32_t insn) noexcept {
    return ux(sx(insn) >> 25) << 5 | (insn >> 7 & 0x1f);
}
std::uint32_t imm_b(std::uint32_t insn) noexcept {
    return ux(sx(insn) >> 31) << 12 | (insn << 4 & 0x800) | (insn >> 20 & 0x7e0) |
           (insn >> 7 & 0x1e);
}
std::uint32_t imm_u(std::uint32_t insn) noexcept { return insn & 0xfffff000; }
std::uint32_t imm_j(std::uint32_t insn) noexcept {
    return ux(sx(insn) >> 31) << 20 | (insn & 0xff000) | (insn >> 9 & 0x800) | (insn >> 20 & 0x7fe);
}

// RV32M. Division by zero and signed overflow have defined results (unprivileged ISA 13.2).
std::uint32_t muldiv(unsigned funct3, std::uint32_t a, std::uint32_t b) noexcept {
    constexpr std::uint32_t kIntMin = 0x80000000;
    switch (funct3) {
    case 0: // mul
        return a * b;
    case 1: // mulh
        return ux(std::int64_t{sx(a)} * std::int64_t{sx(b)} >> 32);
    case 2: // mulhsu
        return ux(std::int64_t{sx(a)} * std::int64_t{b} >> 32);
    case 3: // mulhu
        return static_cast<std::uint32_t>(std::uint64_t{a} * std::uint64_t{b} >> 32);
    case 4: // div
        if (b == 0) {
            return 0xffffffff;
        }
        return a == kIntMin && b == 0xffffffff ? kIntMin : ux(sx(a) / sx(b));
    case 5: // divu
        return b == 0 ? 0xffffffff : a / b;
    case 6: // rem
        if (b == 0) {
            return a;
        }
        return a == kIntMin && b == 0xffffffff ? 0 : ux(sx(a) % sx(b));
    default: // remu
        return b == 0 ? a : a % b;
    }
}

// OP and OP-IMM with funct7 0x00/0x20 (only add/sub and srl/sra use 0x20). nullopt: reserved.
std::optional<std::uint32_t> alu(unsigned funct3, bool alt, std::uint32_t a, std::uint32_t b) {
    const unsigned shamt = b & 0x1f;
    switch (funct3) {
    case 0:
        return alt ? a - b : a + b;
    case 1:
        return alt ? std::nullopt : std::optional(a << shamt);
    case 2:
        return alt ? std::nullopt : std::optional<std::uint32_t>(sx(a) < sx(b));
    case 3:
        return alt ? std::nullopt : std::optional<std::uint32_t>(a < b);
    case 4:
        return alt ? std::nullopt : std::optional(a ^ b);
    case 5:
        return alt ? ux(sx(a) >> shamt) : a >> shamt;
    case 6:
        return alt ? std::nullopt : std::optional(a | b);
    default:
        return alt ? std::nullopt : std::optional(a & b);
    }
}

} // namespace

Machine::Machine(const Image& image, std::uint32_t ram_size)
    : ram_(ram_size), pc_(image.entry), brk_(image.brk), brk_min_(image.brk) {
    for (const Segment& s : image.segments) {
        std::ranges::copy(s.data, ram_.begin() + s.vaddr);
    }
    x_[2] = ram_size;
}

bool Machine::in_ram(std::uint32_t addr, std::uint32_t size) const noexcept {
    return std::uint64_t{addr} + size <= ram_.size();
}

std::uint32_t Machine::load(std::uint32_t addr, unsigned size) const {
    std::uint32_t v = 0;
    for (unsigned i = size; i-- > 0;) {
        v = v << 8 | ram_[addr + i];
    }
    return v;
}

void Machine::store(std::uint32_t addr, unsigned size, std::uint32_t value) {
    for (unsigned i = 0; i < size; ++i) {
        ram_[addr + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void Machine::set(unsigned rd, std::uint32_t value) noexcept {
    if (rd != 0) {
        x_[rd] = value;
    }
}

std::optional<std::uint32_t> Machine::peek32(std::uint32_t addr) const {
    return in_ram(addr, 4) ? std::optional(load(addr, 4)) : std::nullopt;
}

std::optional<Stop> Machine::ecall(core::Io& io) {
    const std::uint32_t a0 = x_[10];
    const std::uint32_t a1 = x_[11];
    const std::uint32_t a2 = x_[12];
    switch (x_[17]) {
    case kSysRead: {
        if (a0 != 0) {
            x_[10] = kEbadf;
            return std::nullopt;
        }
        if (!in_ram(a1, a2)) {
            x_[10] = kEfault;
            return std::nullopt;
        }
        // One byte at a time until count or EOF, like rv-emu's `In` loop: never a short read
        // before EOF.
        if (!io.flush()) {
            return fault("E_IO: cannot write to stdout");
        }
        std::uint32_t n = 0;
        for (; n < a2; ++n) {
            const int b = io.read_byte();
            if (b == core::kIoError) {
                return fault("E_IO: cannot read from stdin");
            }
            if (b == core::kEof) {
                break;
            }
            ram_[a1 + n] = static_cast<std::uint8_t>(b);
        }
        x_[10] = n;
        return std::nullopt;
    }
    case kSysWrite: {
        if (a0 != 1 && a0 != 2) {
            x_[10] = kEbadf;
            return std::nullopt;
        }
        if (!in_ram(a1, a2)) {
            x_[10] = kEfault;
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < a2; ++i) {
            if (!io.write_byte(ram_[a1 + i])) {
                return fault("E_IO: cannot write to stdout");
            }
        }
        x_[10] = a2;
        return std::nullopt;
    }
    case kSysExit:
        return Stop{Stop::Kind::Exit, static_cast<int>(a0 & 0xff), {}};
    case kSysBrk:
        // Linux semantics: a request outside [initial break, end of RAM] leaves the break alone;
        // the result is the break after the call.
        if (a0 >= brk_min_ && a0 <= ram_.size()) {
            brk_ = a0;
        }
        x_[10] = brk_;
        return std::nullopt;
    default:
        x_[10] = kEnosys;
        return std::nullopt;
    }
}

std::optional<Stop> Machine::step(core::Io& io) {
    if (!in_ram(pc_, 4)) {
        return fault("instruction fetch outside guest RAM");
    }
    const std::uint32_t insn = load(pc_, 4);
    const unsigned rd = insn >> 7 & 0x1f;
    const unsigned funct3 = insn >> 12 & 0x7;
    const std::uint32_t funct7 = insn >> 25;
    const std::uint32_t a = x_[insn >> 15 & 0x1f];
    const std::uint32_t b = x_[insn >> 20 & 0x1f];
    const auto illegal = [&] { return fault("illegal instruction " + hex(insn)); };
    const auto jump = [&](std::uint32_t target) -> std::optional<Stop> {
        if (target % 4 != 0) {
            return fault("misaligned jump target " + hex(target));
        }
        set(rd, pc_ + 4);
        pc_ = target;
        return std::nullopt;
    };
    std::uint32_t next = pc_ + 4;

    switch (insn & 0x7f) {
    case 0x37: // lui
        set(rd, imm_u(insn));
        break;
    case 0x17: // auipc
        set(rd, pc_ + imm_u(insn));
        break;
    case 0x6f: // jal
        if (auto stop = jump(pc_ + imm_j(insn))) {
            return stop;
        }
        ++steps_;
        return std::nullopt;
    case 0x67: // jalr
        if (funct3 != 0) {
            return illegal();
        }
        if (auto stop = jump((a + imm_i(insn)) & ~1u)) {
            return stop;
        }
        ++steps_;
        return std::nullopt;
    case 0x63: { // beq bne - - blt bge bltu bgeu
        bool taken = false;
        switch (funct3) {
        case 0:
            taken = a == b;
            break;
        case 1:
            taken = a != b;
            break;
        case 4:
            taken = sx(a) < sx(b);
            break;
        case 5:
            taken = sx(a) >= sx(b);
            break;
        case 6:
            taken = a < b;
            break;
        case 7:
            taken = a >= b;
            break;
        default:
            return illegal();
        }
        if (taken) {
            next = pc_ + imm_b(insn);
            if (next % 4 != 0) {
                return fault("misaligned jump target " + hex(next));
            }
        }
        break;
    }
    case 0x03: { // lb lh lw - lbu lhu
        const unsigned size = 1u << (funct3 & 3);
        if (funct3 == 3 || funct3 > 5) {
            return illegal();
        }
        const std::uint32_t addr = a + imm_i(insn);
        if (!in_ram(addr, size)) {
            return fault("load from " + hex(addr) + " outside guest RAM");
        }
        std::uint32_t v = load(addr, size);
        if (funct3 < 2) { // sign-extend lb, lh
            const unsigned shift = 32 - 8 * size;
            v = ux(sx(v << shift) >> shift);
        }
        set(rd, v);
        break;
    }
    case 0x23: { // sb sh sw
        if (funct3 > 2) {
            return illegal();
        }
        const unsigned size = 1u << funct3;
        const std::uint32_t addr = a + imm_s(insn);
        if (!in_ram(addr, size)) {
            return fault("store to " + hex(addr) + " outside guest RAM");
        }
        store(addr, size, b);
        break;
    }
    case 0x13: { // addi slti sltiu xori ori andi slli srli srai
        const std::uint32_t imm = imm_i(insn);
        // Only the shifts look at funct7; srai is the one with 0x20. imm[5] set is reserved.
        const bool shift = funct3 == 1 || funct3 == 5;
        if (shift && funct7 != 0 && !(funct3 == 5 && funct7 == 0x20)) {
            return illegal();
        }
        set(rd, *alu(funct3, shift && funct7 == 0x20, a, imm));
        break;
    }
    case 0x33: { // add sub sll slt sltu xor srl sra or and; mul..remu
        if (funct7 == 0x01) {
            set(rd, muldiv(funct3, a, b));
            break;
        }
        const auto v =
            funct7 == 0 || funct7 == 0x20 ? alu(funct3, funct7 == 0x20, a, b) : std::nullopt;
        if (!v) {
            return illegal();
        }
        set(rd, *v);
        break;
    }
    case 0x0f: // fence, fence.i: no-ops; there is one hart and fetch reads RAM every time
        if (funct3 > 1) {
            return illegal();
        }
        break;
    case 0x73: // ecall
        if (insn == 0x00100073) {
            return fault("ebreak");
        }
        if (insn != 0x00000073) {
            return illegal();
        }
        if (auto stop = ecall(io)) {
            if (stop->kind == Stop::Kind::Exit) {
                ++steps_;
            }
            return stop;
        }
        break;
    default:
        return illegal();
    }
    pc_ = next;
    ++steps_;
    return std::nullopt;
}

Stop Machine::run(core::Io& io) {
    std::optional<Stop> stop;
    while (!(stop = step(io))) {
    }
    if (!io.flush() && stop->kind == Stop::Kind::Exit) {
        return fault("E_IO: cannot write to stdout");
    }
    return *stop;
}

} // namespace malm::rv
