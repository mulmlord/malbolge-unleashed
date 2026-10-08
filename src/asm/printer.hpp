#pragma once

// Canonical pretty-printer: AST -> source (docs/asm.md). The output parses back to an AST that is
// structurally equal to the printed one (same_structure in ast.hpp), which is what the round-trip
// tests check.
//
// Decisions:
//  * Includes are flattened: the blocks of included files are printed inline, in order, and there
//    is no `.INCLUDE` in the output (the AST holds only the expanded program). Comments are not
//    kept.
//  * A `.CODE` / `.DATA` line is printed before the first block and whenever the section changes;
//    blocks are separated by one blank line; a braced block is printed with braces; `.OFFSET` is
//    printed as `@expr`.
//  * Cells are grouped into lines as in the source (Cell::starts_line, docs/asm.md 2): a line holds
//    its cells separated by single spaces, labels first. A string was expanded by the parser into
//    one cell per byte and is printed as such, one word per byte, all on the string's line.
//  * Literals: `'c'` when the AST says it was a character, a decimal for other head-0 values and
//    `...h<trits>` for heads 1 and 2. Operators are written without spaces, parentheses only where
//    the precedence needs them. A word that would start with a unary minus is wrapped in
//    parentheses, because a leading `-` on a new line would otherwise continue the previous word
//    (the extent rule of docs/asm.md 4.3).

#include <string>

#include "asm/ast.hpp"

namespace malm::assembler {

std::string print_expr(const Expr& e);
std::string print_program(const Program& p);

} // namespace malm::assembler
