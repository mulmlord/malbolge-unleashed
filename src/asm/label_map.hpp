#pragma once

// The label map (docs/asm.md 7, docs/cli.md): written by `malm asm --map`, read by `malm disasm`.
// One line per label, `name address code|data first last`, ordered by address and then by name;
// `first` and `last` are the extent of the label's block. Addresses are in the notation of spec 1
// (`0t...`, `...1...`).

#include <string>
#include <string_view>
#include <vector>

#include "asm/ast.hpp"
#include "asm/layout.hpp"
#include "core/result.hpp"

namespace malm::assembler {

struct MapEntry {
    std::string name;
    Value addr;
    Section section = Section::Code;
    Value first; // the extent of the label's block
    Value last;
};

std::string format_label_map(const Layout& layout);

// Parses a label map. Errors name the line: "line 3: ...".
core::Result<std::vector<MapEntry>, std::string> parse_label_map(std::string_view text);

} // namespace malm::assembler
