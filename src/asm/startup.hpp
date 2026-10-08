#pragma once

// Startup code (docs/asm.md 7, issue #15): the straight-line code that builds the memory image of a
// laid-out program, forces n to the program width and jumps to ENTRY. Programs are pure (spec 4),
// so every image cell is written by this code at run time.
//
// Program text. Cell 0 is the Jmp `b`, which continues at cell 99 with D = 1. Cells 1..98 hold the
// constants and pointers of a small work area (the "low area"), cells 99.. the startup code, then
// filler up to the last two cells. Those are two characters whose trits are all 0 or 1, the last
// one y = 85, at an odd length L. The fill (spec 2.4) then alternates: a never-written cell holds
// y at an even remainder mod 6 and ~y (0 and 1 trits swapped, head 1) at an odd one. Every user
// block lies at or above `base` >= L (layout.hpp), so every image cell starts out as fill.
//
// Returning from a target. Writing cell X leaves D on X + 1, a fill cell. A MovD there lands on
// y + 1 = 86, a pointer back into the low area, or on ~y + 1, whose fill is y and which leads there
// in one more MovD.
//
// The work area. D moves between the registers of the low area along fixed pointers (a static
// graph, routed by BFS); moving never changes A. The registers:
//   C0    0, a rotation fixed point: Rot loads A = 0 without changing it.
//   Z0/Z1 the low n trits 2, head 0/1: also fixed points. With A = Z, Opr swaps the trits 1 and 2
//         of the register (S below) and sets its head to 1/0.
//   SEED  the low n trits 2 except one "hot" trit 1, head 1. Every Rot moves the hot trit down by
//         one position (cyclically). With A = SEED, Opr acts like S, except at the hot position.
//   ACC_V the value the next Opr on a target uses; ACC_P the pointer to the target's base.
//   Q, F1, F2 used by the bootstrap only.
//
// Building a value (Generator::plan). Track a register in the frame X = S^k(register) after k
// S-type operations. Its trits are first mapped into {1, 2} by one Opr with A = C0 if needed. An
// Opr with A = SEED then changes only the hot position in the frame, by kappa = (0 0 1) after an
// even number of S-type operations and kappa' = (0 2 0) after an odd one; an Opr with A = Z0
// changes nothing in the frame but the parity. So the hot trit visits the positions one by one, and
// each position is fixed by at most one insertion, preceded by a Z0 operation when the parity is
// wrong. A last Z operation picks the head. That is about 2 steps per untouched and 5 per changed
// trit.
//
// Writing a target X with value V (Generator::write_target). ACC_P holds B - 1 for a base B <= X
// close below X (rebuilt when X is too far away), and ACC_V a value a with crz(a, [X]) = V. A MovD
// at ACC_P and X - B Nops bring D to X, and an Opr writes V. If [X] has a trit 0 where V has a
// trit 2 (no such a exists), a first trip with A = 0 turns [X] into ...111.
//
// Bootstrap. A starts at 0, so one Opr turns a 0/1-trit character into ...111 (C1) and the next
// one, with A = C1, another into 0 (C0). Width forcing (docs/asm.md 7): while n < N, F1/F2 take a
// pointer swap01(rotr(f)) of width n, built from a rotated fill cell f, and a MovD through it grows
// n by exactly one. Then Z1 is built at the final width by adding the 2 of a rotating Q = 54 (one
// trit 2, otherwise 0) once per position, and Z0 and SEED are derived from Z1 and Q.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "asm/layout.hpp"

namespace malm::assembler {

struct Startup {
    std::vector<std::uint8_t> text; // the program text: cells 0..L-1
    // Steps after which C is at the final Jmp of the startup code and D = ENTRY (docs/asm.md 5.3).
    std::uint64_t steps = 0;
    std::uint64_t code_cells = 0; // instructions of the startup code, from cell 99 on
};

// Initial n of a program text of `length` cells (spec 2.2).
unsigned initial_width(std::uint64_t length);

// Generates the startup code for `layout`, whose head-0 blocks lie at or above `base`, assuming the
// text will start with n = `initial_width` (the caller checks that against the text's length and
// that the text ends at or below `base`). On failure `error` says why and the result is nullopt.
std::optional<Startup> generate_startup(const Layout& layout, std::uint64_t base,
                                        unsigned initial_width, std::string& error);

} // namespace malm::assembler
