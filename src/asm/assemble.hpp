#pragma once

// Assembly of a checked program into its program text (docs/asm.md 7, 8.1): layout plus startup
// code. The two depend on each other: the startup code is as long as the image needs, the user
// blocks start where the text ends (`base`), and the text's length sets the initial n, which must
// not exceed the program width. So the search lays out at a base, generates the startup code, and
// repeats with a higher base or width until the text ends at or below the base and fits the width.

#include <cstdint>
#include <optional>

#include "asm/diagnostics.hpp"
#include "asm/layout.hpp"
#include "asm/startup.hpp"

namespace malm::assembler {

struct Assembly {
    Layout layout;
    Startup startup;
    std::uint64_t base = 0; // first address of the head-0 window
};

// Errors go to `diags`; the result is then nullopt.
std::optional<Assembly> assemble(const Program& program, Diagnostics& diags);

} // namespace malm::assembler
