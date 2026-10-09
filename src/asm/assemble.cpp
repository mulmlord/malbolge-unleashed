#include "asm/assemble.hpp"

#include <algorithm>
#include <string>

namespace malm::assembler {

std::optional<Assembly> assemble(const Program& program, Diagnostics& diags) {
    const SourceLoc root{0, 1, 1};
    LayoutOptions options;
    for (int round = 0; round < 64; ++round) {
        auto layout = layout_program(program, diags, options);
        if (!layout) {
            return std::nullopt;
        }
        // The initial n the code assumes must be the one its text gives.
        // The text is padded up to the n0 it was generated for, so n0 only grows here.
        unsigned n0 = initial_width(options.base);
        std::optional<Startup> startup;
        std::string error;
        for (int tries = 0;; ++tries) {
            startup = generate_startup(*layout, options.base, n0, error);
            if (!startup || initial_width(startup->text.size()) == n0) {
                break;
            }
            if (tries == 8) {
                startup.reset();
                error = "the initial n of the text does not converge";
                break;
            }
            n0 = initial_width(startup->text.size());
        }
        const bool too_wide = n0 > layout->width;
        if (!too_wide && !startup) {
            diags.error(root, "cannot generate the startup code: " + error);
            return std::nullopt;
        }
        if (too_wide) {
            if (program.width) {
                diags.error(
                    program.width->loc,
                    "the program does not fit at width " + std::to_string(layout->width) +
                        ": its startup code makes the program text need n = " + std::to_string(n0));
                return std::nullopt;
            }
            options.min_width = layout->width + 1;
            continue;
        }
        const std::uint64_t length = startup->text.size();
        if (length <= options.base) {
            return Assembly{std::move(*layout), std::move(*startup), options.base};
        }
        // Leave some room: the next layout's addresses, and so its startup code, may grow. A block
        // pinned in head 0 caps the room, so that a program whose blocks are all pinned (e.g. the
        // output of `malm disasm`) keeps its layout; the text itself must still end below it.
        std::uint64_t next = length + length / 16 + 16;
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            const Value start = layout->blocks[b].footprint;
            if (program.blocks[b].offset && start.head() == 0 && start.raw() >= length) {
                next = std::min(next, start.raw());
            }
        }
        options.base = next;
        options.min_width = layout->width;
    }
    diags.error(root, "the layout and the startup code do not converge");
    return std::nullopt;
}

} // namespace malm::assembler
