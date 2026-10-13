#include "aot/cfg.hpp"

#include <array>
#include <format>
#include <optional>

namespace malm::aot {
namespace {

std::int32_t sx(std::uint32_t v) noexcept { return static_cast<std::int32_t>(v); }
std::uint32_t ux(std::int32_t v) noexcept { return static_cast<std::uint32_t>(v); }

// The code: the 4-aligned words that lie wholly inside an executable segment (bytes past the
// file's are zero).
std::map<std::uint32_t, Insn> code_words(const rv::Image& image) {
    std::map<std::uint32_t, Insn> code;
    for (const rv::Segment& s : image.segments) {
        if (!s.exec) {
            continue;
        }
        const std::uint64_t end = std::uint64_t{s.vaddr} + s.memsz;
        for (std::uint64_t a = (s.vaddr + 3ull) & ~3ull; a + 4 <= end; a += 4) {
            std::uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const std::uint64_t at = a + i - s.vaddr;
                if (at < s.data.size()) {
                    word |= std::uint32_t{s.data[at]} << (8 * i);
                }
            }
            code.emplace(static_cast<std::uint32_t>(a), Insn{word});
        }
    }
    return code;
}

// Whether the instruction writes rd (ecall writes a0, see below).
bool writes_rd(const Insn& in) {
    switch (in.opcode()) {
    case kOpLui:
    case kOpAuipc:
    case kOpJal:
    case kOpJalr:
    case kOpOp:
    case kOpImm:
    case kOpLoad:
        return true;
    default:
        return false;
    }
}

class Recovery {
  public:
    explicit Recovery(const rv::Image& image) : image_(image) { cfg_.code = code_words(image); }

    core::Result<Cfg, std::string> run() {
        using R = core::Result<Cfg, std::string>;
        for (const auto& [pc, in] : cfg_.code) {
            if (in.opcode() == kOpMiscMem && in.funct3() == 1) {
                return R::err(std::format("fence.i at 0x{:x}: self-modifying code is not "
                                          "supported in aot mode",
                                          pc));
            }
        }
        indirect(image_.entry);
        direct_flow();
        scan_data();
        // Constants can add leaders, and a leader ends the block whose constants a jalr used:
        // repeat until nothing changes.
        do {
            changed_ = false;
            cfg_.resolved.clear();
            if (auto e = propagate_constants()) {
                return R::err(std::move(*e));
            }
        } while (changed_);
        return R::ok(std::move(cfg_));
    }

  private:
    bool is_code(std::uint32_t a) const { return cfg_.code.contains(a); }

    void leader(std::uint32_t a) {
        if (is_code(a) && cfg_.leaders.insert(a).second) {
            changed_ = true;
        }
    }
    void indirect(std::uint32_t a) {
        if (is_code(a)) {
            leader(a);
            cfg_.indirect.insert(a);
        }
    }

    // Branch and jump targets and the instructions after control transfers.
    void direct_flow() {
        for (const auto& [pc, in] : cfg_.code) {
            switch (in.opcode()) {
            case kOpBranch:
                leader(pc + in.imm_b());
                leader(pc + 4);
                break;
            case kOpJal:
            case kOpJalr:
                if (in.opcode() == kOpJal) {
                    leader(pc + in.imm_j());
                }
                if (in.rd() != 0) {
                    indirect(pc + 4); // the return address
                } else {
                    leader(pc + 4);
                }
                break;
            default:
                break;
            }
        }
    }

    // Code addresses stored in the image: jump tables, function pointers, .init_array. An
    // instruction word is never one (its low two bits are 11), so every segment is scanned.
    void scan_data() {
        for (const rv::Segment& s : image_.segments) {
            for (std::size_t i = (4 - s.vaddr % 4) % 4; i + 4 <= s.data.size(); i += 4) {
                indirect(std::uint32_t{s.data[i]} | std::uint32_t{s.data[i + 1]} << 8 |
                         std::uint32_t{s.data[i + 2]} << 16 | std::uint32_t{s.data[i + 3]} << 24);
            }
        }
    }

    // Per block, the registers set by lui, auipc and addi from constants. A constant that is a
    // code address is an indirect target (`la a0, f`); a jalr on a constant is resolved; a store
    // to a constant address in the code is self-modifying.
    std::optional<std::string> propagate_constants() {
        std::array<std::optional<std::uint32_t>, 32> k{};
        std::optional<std::uint32_t> prev;
        for (const auto& [pc, in] : cfg_.code) {
            if (cfg_.leaders.contains(pc) || prev != pc - 4) {
                k.fill(std::nullopt);
            }
            prev = pc;
            k[0] = 0;
            const std::optional<std::uint32_t> base = k[in.rs1()];
            std::optional<std::uint32_t> value;
            switch (in.opcode()) {
            case kOpLui:
                value = in.imm_u();
                break;
            case kOpAuipc:
                value = pc + in.imm_u();
                break;
            case kOpImm:
                if (in.funct3() == 0 && base) {
                    value = *base + in.imm_i();
                }
                break;
            case kOpJalr:
                if (in.funct3() == 0 && base) {
                    const std::uint32_t target = (*base + in.imm_i()) & ~1u;
                    cfg_.resolved[pc] = target;
                    leader(target);
                }
                break;
            case kOpStore:
                if (base && in.funct3() <= 2) { // sb, sh, sw
                    const std::uint32_t ea = *base + in.imm_s();
                    const std::uint32_t last = ea + (1u << in.funct3()) - 1;
                    if (is_code(ea & ~3u) || is_code(last & ~3u)) {
                        return std::format("store to 0x{:x} at 0x{:x}: self-modifying code is "
                                           "not supported in aot mode",
                                           ea, pc);
                    }
                }
                break;
            case kOpSystem:
                k[10] = std::nullopt; // ecall's result
                break;
            default:
                break;
            }
            if (writes_rd(in)) {
                k[in.rd()] = value;
                if (value && in.rd() != 0) {
                    indirect(*value);
                }
            }
        }
        return std::nullopt;
    }

    const rv::Image& image_;
    Cfg cfg_;
    bool changed_ = false;
};

} // namespace

std::uint32_t Insn::imm_i() const noexcept { return ux(sx(word) >> 20); }
std::uint32_t Insn::imm_s() const noexcept {
    return ux(sx(word) >> 25) << 5 | (word >> 7 & 0x1f);
}
std::uint32_t Insn::imm_b() const noexcept {
    return ux(sx(word) >> 31) << 12 | (word << 4 & 0x800) | (word >> 20 & 0x7e0) |
           (word >> 7 & 0x1e);
}
std::uint32_t Insn::imm_j() const noexcept {
    return ux(sx(word) >> 31) << 20 | (word & 0xff000) | (word >> 9 & 0x800) | (word >> 20 & 0x7fe);
}

core::Result<Cfg, std::string> recover_cfg(const rv::Image& image) { return Recovery(image).run(); }

} // namespace malm::aot
