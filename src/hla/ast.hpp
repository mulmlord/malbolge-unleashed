#pragma once

// AST of the high-level assembly language (docs/hla.md). Plain value types; locations and
// diagnostics are the assembler's, so diagnostics of both languages look the same.

#include <cstdint>
#include <optional>
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
    std::string name;     // Name: a register, a label or a RAM region
    std::int64_t imm = 0; // Imm: as written, -2^31 .. 2^32 - 1
    std::string text;     // String: the bytes
    bool mem = false;     // written in brackets: a RAM address (Name or Imm)
};

enum class Op {
    Label,
    Mov,
    Jmp,
    Bz,
    Bnz,
    In,
    Out,
    Halt,
    Call,
    Ret,
    Push,
    Pop,
    Switch,
    Load,
    Store,
    // Arithmetic (#21): `op d, x, y` with y a register or a value; sextb/sexth `op d, x`.
    Add,
    Sub,
    And,
    Or,
    Xor,
    Slt,
    Sltu,
    Sll,
    Srl,
    Sra,
    Sextb,
    Sexth,
    // Multiplication and division (#22, RV32M): `op d, x, y` with y a register or a value.
    Mul,
    Mulh,
    Mulhsu,
    Mulhu,
    Div,
    Divu,
    Rem,
    Remu
};

// One statement, or a label definition (Op::Label, `name` set, no operands).
struct Stmt {
    Op op = Op::Label;
    SourceLoc loc; // the mnemonic, or the label
    std::string name;
    std::vector<Operand> args;
};

struct RegDecl {
    std::string name;
    Operand init; // a value (docs/hla.md 3); Imm 0 if not written
    SourceLoc loc;
};

// `ram name[size] = v, ...`: `size` words from the byte address `base`, the first ones set to
// `init`, the others 0.
struct RamDecl {
    std::string name;
    std::uint32_t size = 0;
    std::uint32_t base = 0;
    std::vector<Operand> init;
    SourceLoc loc;
};

struct StackDecl {
    std::uint32_t size = 0;
    SourceLoc loc;
};

struct Program {
    std::vector<SourceFile> files; // one file: HLA has no includes
    std::vector<RegDecl> regs;
    std::vector<RamDecl> rams;
    std::optional<StackDecl> stack;
    std::vector<Stmt> stmts;
};

inline bool is_shift(Op op) { return op == Op::Sll || op == Op::Srl || op == Op::Sra; }
inline bool is_muldiv(Op op) { return op >= Op::Mul && op <= Op::Remu; }

// The 32-bit word an immediate stands for (two's complement for negative values).
inline std::uint32_t imm_word(std::int64_t imm) { return static_cast<std::uint32_t>(imm); }

} // namespace malm::hla
