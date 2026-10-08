#pragma once

// Layout (docs/asm.md 7, issue #14): places every block in memory, picks the character of every
// code cell, adds the reserved predecessor cells and the `U_` RNop runs, evaluates the data words,
// and returns the memory image the startup code (#15) has to establish.
//
// Footprint. A code block occupies its reserved cell, then its `U_` RNop run (k_max cells), then
// its cells; a data block occupies its cells. Cells are hard (they hold a specific value) or soft
// (an RNop, a reserved cell or a run cell). Soft cells may coincide with other soft cells, since
// each of them only needs a character fixed by its position: an RNop is rnop_char(p), and a
// reserved cell takes the RNop when one is there. A `?-` slot claims nothing, so any cell of
// another block may sit on it. Everything else is exclusive.
//
// Placement. Pinned (`@`) blocks go exactly where they are pinned. The others are placed first-fit
// in source order in the head-0 window [base, 3^(n-1) - 1): the lowest footprint start whose cells
// are free and where every code cell's cycle exists at its position (cycles.hpp). Placement is
// deterministic. `base` is where the program text ends (startup.hpp); the last address of a window
// is left out because writing it would move D past the window, which grows n.
//
// Head 2. crz maps heads 0 and 1 to heads 0 and 1, rotation keeps the head, and the loader and
// the fill produce only heads 0 and 1, so no cell ever holds a head-2 value unless In returned EOF.
// C and D are loaded from cells, so they never reach a head-2 address either. Data words with a
// head-2 value and blocks pinned in head 2 are therefore errors (docs/asm.md 4.3, 7).
//
// Characters (docs/asm.md 3.1, 7). A single command is the loadable character for it at the
// position. A cycle is the smallest loadable character whose xlat2 cycle matches it there, or the
// smallest matching character if none is loadable. An RNop or all-Nop cycle is rnop_char. A
// reserved cell is the loadable `o` (Nop) at its position.
//
// Width. `.WIDTH n` is used as given. Without it the layout starts at the smallest n (at least
// `min_width`) the cells could fit in and retries with n + 1 while some address does not fit, up
// to 39 (docs/asm.md 8.1; assemble.hpp adds the startup code to that search).
//
// Layout checks reported here: impossible code positions, pinned blocks outside a window or
// overlapping, blocks that do not fit, expression errors, data words wider than n, and the
// cell-0 and pointer-safety rules for constant words in code-reference positions (the first word
// of a data line, docs/asm.md 6).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "asm/ast.hpp"
#include "asm/diagnostics.hpp"

namespace malm::assembler {

// Lowest possible first address of the head-0 window: cells 0..98 always belong to the startup code
// and cell 99 starts it (docs/asm.md 7).
inline constexpr std::uint64_t kFirstUserAddress = 100;

struct LayoutOptions {
    unsigned min_width = 10;                // ignored with `.WIDTH`
    std::uint64_t base = kFirstUserAddress; // first address of the head-0 window
};

enum class CellRole : std::uint8_t {
    Code,     // a code word
    Data,     // a data word with a defined value (not `?` or `?-`)
    Reserved, // the reserved predecessor cell of a code block or of its U_ run
    RNopRun,  // a cell of a U_ RNop run
};

struct ImageCell {
    Value addr;
    Value value;
    CellRole role = CellRole::Code;
    // The value is a character loadable at the cell's position, so the cell could be part of the
    // loaded program text (docs/asm.md 7, "Preinitialized vs. runtime-written").
    bool loadable = false;
};

struct LabelInfo {
    Value addr;
    Section section = Section::Code;
    std::size_t block = 0; // index into Program::blocks
};

struct PlacedBlock {
    Value first;     // the block's first cell
    Value last;      // its last cell
    Value footprint; // the first cell of its footprint: a code block's reserved cell
};

struct Layout {
    unsigned width = 0;                                // the program width n
    std::vector<PlacedBlock> blocks;                   // parallel to Program::blocks
    std::unordered_map<std::string, LabelInfo> labels; // every label's address
    // One entry per address with a defined initial value, ordered by raw address (head 0, then
    // head 1, then head 2). `?` cells and `?-` slots have none.
    std::vector<ImageCell> cells;

    // The image cell at `addr`, or null.
    const ImageCell* find(Value addr) const;
};

// The character code word `w` assembles to at position p (mod 94), or 0 if its cycle does not
// exist there (the rule above).
std::uint8_t word_char(const Word& w, std::size_t p);

// Lays out a program that passed check_program. Errors go to `diags`; the result is then nullopt.
std::optional<Layout> layout_program(const Program& program, Diagnostics& diags,
                                     const LayoutOptions& options = {});

} // namespace malm::assembler
