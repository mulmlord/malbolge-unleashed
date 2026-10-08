#pragma once

// Static checks (docs/asm.md 9): everything decidable from the AST alone, before layout.
//
//  * duplicate and unknown labels; every plain, `R_` and `U_` reference must resolve
//  * `R_X`: X is a code label naming a cycle cell that is not the last cell of its block
//  * `U_X Y`: X names the first cell of a code block, Y a data label of a later cell in the same
//    data block (the distance rules belong to layout)
//  * `?-`: no label may name it
//  * every block starts with a label
//  * ENTRY: present, defined in a data block, its first word a code reference
//  * `@`/`.OFFSET` expressions are constant (no labels, no `<<` / `>>`)
//  * every cycle exists at some position mod 94 (cycles.hpp)
//
// Width and placement of `.WIDTH`, `.INCLUDE` and the syntax are the parser's.

#include "asm/ast.hpp"
#include "asm/diagnostics.hpp"
#include "asm/parser.hpp"

namespace malm::assembler {

void check_program(const Program& program, Diagnostics& diags);

// Parse, then check unless the parse reported errors (a recovered AST would only cascade).
Program analyze(const std::string& path, FileLoader& loader, Diagnostics& diags,
                const ParseLimits& limits = {});

} // namespace malm::assembler
