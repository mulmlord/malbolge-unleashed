// RV32IM ELF loader (docs/rv.md): validates an ELF32 executable and turns its PT_LOAD segments into
// a guest RAM image. Shared by the reference emulator, the emu-image builder and aot.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"

namespace malm::rv {

// Guest RAM is the byte range [0, ram_size); there is nothing else in the address space.
inline constexpr std::uint32_t kDefaultRamSize = 1u << 20;
inline constexpr std::uint32_t kMaxRamSize = 1u << 30;
// ram_size must be a multiple of this, so the initial sp (= ram_size) is ABI-aligned.
inline constexpr std::uint32_t kRamAlign = 16;

struct Segment {
    std::uint32_t vaddr = 0;
    std::uint32_t memsz = 0;        // bytes past data.size() are zero (.bss)
    std::vector<std::uint8_t> data; // the p_filesz bytes from the file
    bool exec = false;              // PF_X
};

struct Image {
    std::uint32_t entry = 0;
    std::vector<Segment> segments; // sorted by vaddr, non-empty, disjoint, inside guest RAM
    std::uint32_t brk = 0;         // initial program break: end of the highest segment
};

// Accepts a little-endian ELF32 ET_EXEC for EM_RISCV without the RVC, RVE or hardware-float ABI
// flags whose PT_LOAD segments fit in [0, ram_size) without overlapping, and whose entry point is
// 4-aligned inside an executable segment. Other program headers are ignored. The error is a
// one-line reason.
core::Result<Image, std::string> load_elf(std::span<const std::uint8_t> file,
                                          std::uint32_t ram_size = kDefaultRamSize);

} // namespace malm::rv
