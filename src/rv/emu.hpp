// rv-emu (docs/rv.md 8): the RV32IM emulator written in HLA. emu_source() generates its HLA
// program; the build turns it into emu.malm (rv/emu/CMakeLists.txt).
//
// The emulator keeps the guest's registers in the HLA registers x1 .. x31 and pc, and guest RAM in
// the HLA RAM region `mem` at address 0, so a guest address is an HLA address. Each turn of its
// loop, at the label `fetch`, loads the instruction word at pc, dispatches on the opcode, reads
// the operands, runs the instruction's semantics (semantics.hpp), writes rd and advances pc.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "rv/elf.hpp"
#include "rv/semantics.hpp"

namespace malm::rv {

struct EmuOptions {
    std::uint32_t ram_size = kDefaultRamSize; // a multiple of kRamAlign up to kMaxRamSize
    // The guest program: RAM holds its segments, pc starts at its entry and the program break at
    // its end. Without one, RAM is zero and pc starts at 0.
    std::optional<Image> image;
    bool harness = false; // see sem::Config
};

// The emulator's HLA program. sp (x2) starts at ram_size, every other guest register at 0.
// Precondition: an image fits in ram_size (load_elf with the same size).
std::string emu_source(const EmuOptions& opt);

} // namespace malm::rv
