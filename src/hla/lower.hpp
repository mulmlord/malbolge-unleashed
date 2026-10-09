#pragma once

// Lowering of a checked HLA program to an assembly program (docs/hla.md 4).
//
// The result is an assembler AST: `.WIDTH 38`, the code units the program needs, the main data
// stream (ENTRY), one data block per register, the return trees and the dispatch tables. Every
// cell generated for a statement carries the statement's source location, and every support cell
// the location of the declaration or first use it serves, so the assembler's diagnostics point
// into the .mhl file (Program::files is the HLA program's file table).

#include <string>

#include "asm/ast.hpp"
#include "asm/parser.hpp"
#include "hla/ast.hpp"
#include "hla/parser.hpp"

namespace malm::hla {

assembler::Program lower(const Program& program);

// parse_file, lower and the assembler's static checks (asm/check.hpp). With errors in `diags` the
// result is incomplete; its file table is always set.
assembler::Program translate(const std::string& path, assembler::FileLoader& loader,
                             Diagnostics& diags);

} // namespace malm::hla
