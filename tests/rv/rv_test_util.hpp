// Helpers for the RISC-V tests: RV32IM instruction encoders, an in-memory ELF32 builder and a
// core::Io over strings.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/vm.hpp"

namespace rvtest {

using u32 = std::uint32_t;

// Base instruction formats; immediates are taken mod their field width.
constexpr u32 r_type(u32 op, u32 rd, u32 f3, u32 rs1, u32 rs2, u32 f7) {
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
constexpr u32 i_type(u32 op, u32 rd, u32 f3, u32 rs1, std::int32_t imm) {
    return (static_cast<u32>(imm) & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
constexpr u32 s_type(u32 op, u32 f3, u32 rs1, u32 rs2, std::int32_t imm) {
    const u32 v = static_cast<u32>(imm);
    return (v >> 5 & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (v & 0x1f) << 7 | op;
}
constexpr u32 b_type(u32 f3, u32 rs1, u32 rs2, std::int32_t imm) {
    const u32 v = static_cast<u32>(imm);
    return (v >> 12 & 1) << 31 | (v >> 5 & 0x3f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
           (v >> 1 & 0xf) << 8 | (v >> 11 & 1) << 7 | 0x63;
}
constexpr u32 u_type(u32 op, u32 rd, u32 imm20) { return imm20 << 12 | rd << 7 | op; }
constexpr u32 j_type(u32 rd, std::int32_t imm) {
    const u32 v = static_cast<u32>(imm);
    return (v >> 20 & 1) << 31 | (v >> 1 & 0x3ff) << 21 | (v >> 11 & 1) << 20 |
           (v >> 12 & 0xff) << 12 | rd << 7 | 0x6f;
}

constexpr u32 addi(u32 rd, u32 rs1, std::int32_t imm) { return i_type(0x13, rd, 0, rs1, imm); }
constexpr u32 li(u32 rd, std::int32_t imm) { return addi(rd, 0, imm); }
constexpr u32 lui(u32 rd, u32 imm20) { return u_type(0x37, rd, imm20); }
constexpr u32 auipc(u32 rd, u32 imm20) { return u_type(0x17, rd, imm20); }
constexpr u32 op(u32 f3, u32 f7, u32 rd, u32 rs1, u32 rs2) {
    return r_type(0x33, rd, f3, rs1, rs2, f7);
}
constexpr u32 load(u32 f3, u32 rd, u32 rs1, std::int32_t imm) {
    return i_type(0x03, rd, f3, rs1, imm);
}
constexpr u32 store(u32 f3, u32 rs1, u32 rs2, std::int32_t imm) {
    return s_type(0x23, f3, rs1, rs2, imm);
}
constexpr u32 jal(u32 rd, std::int32_t imm) { return j_type(rd, imm); }
constexpr u32 jalr(u32 rd, u32 rs1, std::int32_t imm) { return i_type(0x67, rd, 0, rs1, imm); }
constexpr u32 kEcall = 0x00000073;
constexpr u32 kEbreak = 0x00100073;

// Register numbers.
constexpr u32 sp = 2, t0 = 5, t1 = 6, t2 = 7, a0 = 10, a1 = 11, a2 = 12, a7 = 17;

struct ElfSegment {
    u32 vaddr;
    std::vector<std::uint8_t> data;
    u32 memsz;     // 0: data.size()
    u32 flags = 5; // PF_R | PF_X
    u32 type = 1;  // PT_LOAD
};

struct ElfSpec {
    u32 entry = 0x1000;
    std::vector<ElfSegment> segments;
    std::uint8_t elf_class = 1;
    std::uint8_t data = 1;
    std::uint16_t type = 2;
    std::uint16_t machine = 243;
    u32 flags = 0;
    std::uint16_t phentsize = 32;
};

inline void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline void put32(std::vector<std::uint8_t>& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::vector<std::uint8_t> words(std::initializer_list<u32> ws) {
    std::vector<std::uint8_t> b(ws.size() * 4);
    std::size_t at = 0;
    for (const u32 w : ws) {
        put32(b, at, w);
        at += 4;
    }
    return b;
}

// ELF header, then the program headers, then each segment's data.
inline std::vector<std::uint8_t> build_elf(const ElfSpec& spec) {
    const std::size_t phoff = 52;
    std::size_t offset = phoff + 32 * spec.segments.size();
    std::vector<std::uint8_t> b(offset);
    b[0] = 0x7f;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = spec.elf_class;
    b[5] = spec.data;
    b[6] = 1;
    put16(b, 16, spec.type);
    put16(b, 18, spec.machine);
    put32(b, 20, 1);
    put32(b, 24, spec.entry);
    put32(b, 28, static_cast<u32>(phoff));
    put32(b, 36, spec.flags);
    put16(b, 40, 52);
    put16(b, 42, spec.phentsize);
    put16(b, 44, static_cast<std::uint16_t>(spec.segments.size()));
    for (std::size_t i = 0; i < spec.segments.size(); ++i) {
        const ElfSegment& s = spec.segments[i];
        const std::size_t ph = phoff + 32 * i;
        const u32 filesz = static_cast<u32>(s.data.size());
        put32(b, ph, s.type);
        put32(b, ph + 4, static_cast<u32>(offset));
        put32(b, ph + 8, s.vaddr);
        put32(b, ph + 12, s.vaddr);
        put32(b, ph + 16, filesz);
        put32(b, ph + 20, s.memsz != 0 ? s.memsz : filesz);
        put32(b, ph + 24, s.flags);
        put32(b, ph + 28, 4);
        b.insert(b.end(), s.data.begin(), s.data.end());
        offset += filesz;
    }
    return b;
}

// Input from a string, output into a string; either side can be made to fail.
struct StringIo final : malm::core::Io {
    std::string in;
    std::string out;
    std::size_t pos = 0;
    bool read_error = false;
    bool write_error = false;

    int read_byte() override {
        if (read_error) {
            return malm::core::kIoError;
        }
        return pos < in.size() ? static_cast<unsigned char>(in[pos++]) : malm::core::kEof;
    }
    bool write_byte(std::uint8_t b) override {
        if (write_error) {
            return false;
        }
        out.push_back(static_cast<char>(b));
        return true;
    }
    bool flush() override { return !write_error; }
};

} // namespace rvtest
