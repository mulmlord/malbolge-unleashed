#pragma once

// The disassembler (`malm disasm`, issue #17): a program text back to readable assembly.
//
// Recognition. The text of a program from `malm asm` is only its startup code (docs/asm.md 7): the
// code and data the programmer wrote are written at run time. So the disassembler runs the text in
// the VM. The startup code is straight-line, does no I/O and ends with the only Jmp after cell 0's,
// which jumps through ENTRY (docs/asm.md 5.3). A text is recognised when, within as many steps as
// it has cells and without I/O, Hlt or a fault, execution reaches a second Jmp with D on a written
// cell whose value + 1 was written too. At that point, before the Jmp executes:
//  * the image is every cell outside the text that an Opr wrote, with its current value. That is
//    exactly the layout's image: code cells, reserved cells, RNop runs and data words;
//  * ENTRY is D, and the program width is n.
//
// Blocks. With a label map (label_map.hpp) the blocks are the map's. Without one, code is what the
// D-driven execution model (docs/asm.md 1) reaches statically from ENTRY: a jump through a word w
// lands at w + 1, every cycle state of a code cell is followed (so flags branch both ways), MovD
// continues after its operand's value. Runtime-computed jumps (sleds) are not followed. The rest
// of the image is data. Both ways, a code block grows backwards over a `U_` RNop run that a word of
// the image lands on, and a code block whose cells cannot be spelled as code words giving the same
// characters, or whose reserved cell does not hold what layout would put there, becomes data.
//
// Output. A .mua that re-assembles to the same image (and so, with the same startup code generator,
// to the same text): `.WIDTH n`, every block pinned with `@` at its address, code cells spelled so
// that the assembler picks the same character (docs/asm.md 3.1, 7), data words as label references
// (`X`, `R_X`) where their value points into a block, otherwise as constants. Each code cell is
// annotated with its address, position mod 94, character and xlat2 cycle. The output is parsed and
// laid out in-process and compared with the image; a difference is reported as a warning.
//
// Anything not recognised (and everything with `raw`) is listed cell by cell: address, position,
// character, normalized instruction letter, decoded instruction and xlat2 cycle.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "asm/label_map.hpp"
#include "core/result.hpp"

namespace malm::assembler {

struct DisasmOptions {
    std::string name;                           // shown in the header
    const std::vector<MapEntry>* map = nullptr; // labels and blocks from `malm asm --map`
    bool raw = false;                           // list the text even if it is recognised
    bool classic = false;                       // a classic program: always listed
};

struct Disassembly {
    std::string text;
    bool recognized = false; // false: `text` is a listing of the program text
    // Non-empty if the output does not re-assemble to the recovered image (a disassembler bug, or
    // code that defeats the block recovery).
    std::string warning;
};

// Errors: a label map that does not match the program.
core::Result<Disassembly, std::string> disassemble(std::span<const std::uint8_t> cells,
                                                   const DisasmOptions& options);

} // namespace malm::assembler
