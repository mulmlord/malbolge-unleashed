#pragma once

// AST of the high-level assembly language (docs/hla.md). Plain value types; locations and
// diagnostics are the assembler's, so diagnostics of both languages look the same.

#include <cstdint>
#include <string>
#include <vector>

#include "asm/diagnostics.hpp"

namespace malm::hla {

using assembler::SourceFile;
using assembler::SourceLoc;

struct Operand {
    enum class Kind { Name, Imm, String };

    Kind kind = Kind::Imm;
    SourceLoc loc;
    std::string name;     // Name: a register or a label
    std::int64_t imm = 0; // Imm: as written, -2^31 .. 2^32 - 1
    std::string text;     // String: the bytes
};

enum class Op { Label, Mov, Jmp, Bz, Bnz, In, Out, Halt };

// One statement, or a label definition (Op::Label, `name` set, no operands).
struct Stmt {
    Op op = Op::Label;
    SourceLoc loc; // the mnemonic, or the label
    std::string name;
    std::vector<Operand> args;
};

struct RegDecl {
    std::string name;
    std::uint32_t init = 0;
    SourceLoc loc;
};

struct Program {
    std::vector<SourceFile> files; // one file: HLA has no includes
    std::vector<RegDecl> regs;
    std::vector<Stmt> stmts;
};

// The 32-bit word an immediate stands for (two's complement for negative values).
inline std::uint32_t imm_word(std::int64_t imm) { return static_cast<std::uint32_t>(imm); }

} // namespace malm::hla
