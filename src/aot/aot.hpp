// rv-aot (docs/rv.md 9): translates an RV32IM program into an HLA program, every guest instruction
// into inline HLA, one block per basic block (cfg.hpp). The instruction semantics and the runtime
// (loads, stores, system calls, faults) are rv-emu's (rv/semantics.hpp), so the program keeps the
// guest's registers in x1 .. x31 and guest RAM at HLA address 0, as rv-emu does; but pc is a
// register only for the diagnostics, and decoding happens at build time.
//
// An indirect jump (jalr) looks its target up in a dispatch table, an HLA RAM region after guest
// RAM that maps every word of an aligned block of guest addresses holding the code to the label of
// its basic block, or to a stop for addresses that are no indirect target.
#pragma once

#include <cstdint>
#include <string>

#include "core/result.hpp"
#include "rv/elf.hpp"

namespace malm::aot {

// The HLA program of `image` (from rv::load_elf with the same ram_size, a multiple of
// rv::kRamAlign). The error is a one-line reason: self-modifying code (recover_cfg), or guest RAM
// and the dispatch table exceed HLA's RAM.
core::Result<std::string, std::string> aot_source(const rv::Image& image, std::uint32_t ram_size);

} // namespace malm::aot
