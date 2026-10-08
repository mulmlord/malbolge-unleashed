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
// in source order in the head-0 window [100, 3^(n-1)): the lowest footprint start whose cells are
// free and where every code cell's cycle exists at its position (cycles.hpp). Placement is
// deterministic.
//
// Characters (docs/asm.md 3.1, 7). A single command is the loadable character for it at the
// position. A cycle is the smallest loadable character whose xlat2 cycle matches it there, or the
// smallest matching character if none is loadable. An RNop or all-Nop cycle is rnop_char. A
// reserved cell is the loadable `o` (Nop) at its position.
//
// Width. `.WIDTH n` is used as given. Without it the layout starts at the smallest n the cells
// could fit in and retries with n + 1 while some address does not fit, up to 39 (docs/asm.md 8.1;
// the startup code's share of that search is #15's).
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

// First address of the head-0 window; cells 0..99 belong to the startup code (docs/asm.md 7).
inline constexpr std::uint64_t kFirstUserAddress = 100;

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
    Value first; // the block's first cell
    Value last;  // its last cell
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

// Lays out a program that passed check_program. Errors go to `diags`; the result is then nullopt.
std::optional<Layout> layout_program(const Program& program, Diagnostics& diags);

} // namespace malm::assembler
