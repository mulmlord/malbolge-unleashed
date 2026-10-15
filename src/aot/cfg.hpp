// rv-aot's view of a guest program (docs/rv.md 9): the instructions of its executable segments,
// the basic blocks they form and the addresses an indirect jump may reach.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "core/result.hpp"
#include "rv/elf.hpp"

namespace malm::aot {

// RV32IM major opcodes (the low 7 bits of an instruction).
inline constexpr unsigned kOpLoad = 0x03;
inline constexpr unsigned kOpMiscMem = 0x0F;
inline constexpr unsigned kOpImm = 0x13;
inline constexpr unsigned kOpAuipc = 0x17;
inline constexpr unsigned kOpStore = 0x23;
inline constexpr unsigned kOpOp = 0x33;
inline constexpr unsigned kOpLui = 0x37;
inline constexpr unsigned kOpBranch = 0x63;
inline constexpr unsigned kOpJalr = 0x67;
inline constexpr unsigned kOpJal = 0x6F;
inline constexpr unsigned kOpSystem = 0x73;

// The fields of an instruction word; immediates sign-extended.
struct Insn {
    std::uint32_t word = 0;

    unsigned opcode() const noexcept { return word & 0x7f; }
    unsigned rd() const noexcept { return word >> 7 & 0x1f; }
    unsigned funct3() const noexcept { return word >> 12 & 0x7; }
    unsigned rs1() const noexcept { return word >> 15 & 0x1f; }
    unsigned rs2() const noexcept { return word >> 20 & 0x1f; }
    unsigned funct7() const noexcept { return word >> 25; }
    std::uint32_t imm_i() const noexcept;
    std::uint32_t imm_s() const noexcept;
    std::uint32_t imm_b() const noexcept;
    std::uint32_t imm_u() const noexcept { return word & 0xfffff000; }
    std::uint32_t imm_j() const noexcept;
};

struct Cfg {
    // The code aot translates: every 4-aligned word inside an executable segment, by address.
    std::map<std::uint32_t, Insn> code;
    // The first instructions of the basic blocks: the entry, the targets of branches and jumps,
    // the instructions after them, and the indirect targets.
    std::set<std::uint32_t> leaders;
    // The leaders an indirect jump may reach, the entries of the dispatch table: the entry, the
    // return addresses of jal and jalr, and code addresses found in the image's words or built by
    // lui, auipc and addi along straight-line code.
    std::set<std::uint32_t> indirect;
    // jalr whose target is a constant (its base built by lui, auipc and addi in its block): the
    // instruction's address -> the target.
    std::map<std::uint32_t, std::uint32_t> resolved;
};

// Recovers the control-flow graph of `image` (from rv::load_elf). The error is a one-line reason
// why aot cannot translate it: self-modifying code, detected by a fence.i or by a store to a
// constant address in the code.
core::Result<Cfg, std::string> recover_cfg(const rv::Image& image);

} // namespace malm::aot
