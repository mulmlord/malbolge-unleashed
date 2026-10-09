#pragma once

// Parser and static checks of the high-level assembly language (docs/hla.md 2, 3).
//
// The language is line-based: a line holds labels (`name:`), then at most one statement or
// register declaration. Errors are reported like the assembler's (`file:line:col: error: ...`);
// after a syntax error the rest of the line is skipped, so independent errors on later lines are
// all reported. The name checks (unknown, duplicate, wrong kind) run only on a program without
// syntax errors.

#include <string>
#include <string_view>

#include "asm/diagnostics.hpp"
#include "asm/parser.hpp"
#include "hla/ast.hpp"

namespace malm::hla {

using assembler::Diagnostics;

inline constexpr std::size_t kMaxIdentLength = 64;
inline constexpr std::size_t kMaxSourceBytes = std::size_t{64} << 20;

// Parses and checks `source`, the content of `path` (registered as file 0 in `diags`).
Program parse(std::string_view source, const std::string& path, Diagnostics& diags);

// Reads `path` through `loader` (at most kMaxSourceBytes), then parse().
Program parse_file(const std::string& path, assembler::FileLoader& loader, Diagnostics& diags);

} // namespace malm::hla
