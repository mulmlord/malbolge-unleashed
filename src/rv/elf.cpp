#include "rv/elf.hpp"

#include <algorithm>
#include <cstddef>

namespace malm::rv {
namespace {

// ELF constants (System V gABI, RISC-V psABI).
constexpr std::size_t kEhdrSize = 52;
constexpr std::size_t kPhdrSize = 32;
constexpr std::uint8_t kClass32 = 1;
constexpr std::uint8_t kData2Lsb = 1;
constexpr std::uint8_t kEvCurrent = 1;
constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEmRiscv = 243;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPfX = 1;
constexpr std::uint32_t kEfRiscvRvc = 0x1;
constexpr std::uint32_t kEfRiscvFloatAbi = 0x6;
constexpr std::uint32_t kEfRiscvRve = 0x8;

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}

using LoadResult = core::Result<Image, std::string>;

} // namespace

LoadResult load_elf(std::span<const std::uint8_t> file, std::uint32_t ram_size) {
    const auto fail = [](std::string msg) { return LoadResult::err(std::move(msg)); };
    if (file.size() < kEhdrSize || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' ||
        file[3] != 'F') {
        return fail("not an ELF file");
    }
    if (file[4] != kClass32) {
        return fail("not a 32-bit ELF file");
    }
    if (file[5] != kData2Lsb) {
        return fail("not a little-endian ELF file");
    }
    if (file[6] != kEvCurrent || u32(file, 20) != kEvCurrent) {
        return fail("unknown ELF version");
    }
    if (u16(file, 16) != kEtExec) {
        return fail("not an executable (ET_EXEC) ELF file");
    }
    if (u16(file, 18) != kEmRiscv) {
        return fail("not a RISC-V ELF file");
    }
    const std::uint32_t flags = u32(file, 36);
    if ((flags & kEfRiscvRvc) != 0) {
        return fail("compressed instructions (RVC) are not supported; build with -march=rv32im");
    }
    if ((flags & kEfRiscvFloatAbi) != 0) {
        return fail("hardware floating-point ABI is not supported; build with -mabi=ilp32");
    }
    if ((flags & kEfRiscvRve) != 0) {
        return fail("the RV32E base ISA is not supported");
    }

    const std::uint32_t phoff = u32(file, 28);
    const std::uint16_t phentsize = u16(file, 42);
    const std::uint16_t phnum = u16(file, 44);
    if (phnum != 0 && phentsize != kPhdrSize) {
        return fail("unexpected program header size");
    }
    if (phoff > file.size() || std::size_t{phnum} * kPhdrSize > file.size() - phoff) {
        return fail("program headers extend past the end of the file");
    }

    Image image;
    image.entry = u32(file, 24);
    for (std::size_t i = 0; i < phnum; ++i) {
        const std::size_t ph = phoff + i * kPhdrSize;
        if (u32(file, ph) != kPtLoad) {
            continue;
        }
        const std::uint32_t offset = u32(file, ph + 4);
        const std::uint32_t vaddr = u32(file, ph + 8);
        const std::uint32_t filesz = u32(file, ph + 16);
        const std::uint32_t memsz = u32(file, ph + 20);
        const std::uint32_t pflags = u32(file, ph + 24);
        if (filesz > memsz) {
            return fail("segment " + std::to_string(i) + " has p_filesz > p_memsz");
        }
        if (offset > file.size() || filesz > file.size() - offset) {
            return fail("segment " + std::to_string(i) + " extends past the end of the file");
        }
        if (memsz == 0) {
            continue;
        }
        if (vaddr > ram_size || memsz > ram_size - vaddr) {
            return fail("segment " + std::to_string(i) + " does not fit in guest RAM (" +
                        std::to_string(ram_size) + " bytes)");
        }
        const auto data = file.subspan(offset, filesz);
        image.segments.push_back({vaddr, memsz, {data.begin(), data.end()}, (pflags & kPfX) != 0});
    }
    if (image.segments.empty()) {
        return fail("no loadable segments");
    }

    std::ranges::sort(image.segments, {}, &Segment::vaddr);
    for (std::size_t i = 1; i < image.segments.size(); ++i) {
        const Segment& prev = image.segments[i - 1];
        if (prev.vaddr + prev.memsz > image.segments[i].vaddr) {
            return fail("loadable segments overlap");
        }
    }
    image.brk = image.segments.back().vaddr + image.segments.back().memsz;

    const bool entry_ok = std::ranges::any_of(image.segments, [&](const Segment& s) {
        return s.exec && image.entry >= s.vaddr && image.entry - s.vaddr < s.memsz;
    });
    if (image.entry % 4 != 0 || !entry_ok) {
        return fail("entry point is not 4-aligned inside an executable segment");
    }
    return LoadResult::ok(std::move(image));
}

} // namespace malm::rv
