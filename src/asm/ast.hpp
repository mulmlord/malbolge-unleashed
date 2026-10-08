#pragma once

// AST of the assembly language (docs/asm.md). Everything is plain value types.
//
// Shape:
//  * Program: an optional `.WIDTH`, and the blocks of the root file and of every included file in
//    source order (an include is expanded in place; Block::loc.file tells where a block came from,
//    Program::files holds the include chain).
//  * Block: a section kind, an optional `@`/`.OFFSET` expression, one or more cells, and whether it
//    was written with braces. A block always has at least one cell.
//  * Cell: its labels, exactly one Word, and whether it starts a source line. A string expands into
//  one cell per byte (plus the
//    separator cells of `"ab", w`), all carrying the location of the string.
//  * Word: a code word (single command, cycle, RNop) or a data word (expression, `?`, `?-`,
//    `U_X Y`).
//  * Expr: literals (Value), label references, `R_X` references, unary minus and the five binary
//    operators. Parentheses are not kept; the printer re-derives them from the precedence.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "asm/diagnostics.hpp"
#include "core/value.hpp"
#include "core/vm.hpp"

namespace malm::assembler {

using core::Op;
using core::Value;

enum class Section { Code, Data };

enum class BinOp { Add, Sub, Shl, Shr, Crz };

// Largest height of an expression tree the parser accepts (bounds every recursion over an Expr).
inline constexpr unsigned kMaxExprHeight = 256;

struct Expr {
    enum class Kind { Literal, Label, Restore, Neg, Binary };

    Kind kind = Kind::Literal;
    SourceLoc loc;
    Value value;            // Literal
    bool is_char = false;   // Literal written as 'c' (only affects printing)
    std::string name;       // Label, Restore
    BinOp op = BinOp::Add;  // Binary
    std::vector<Expr> kids; // Neg: 1, Binary: 2 (left, right)
    unsigned height = 1;    // 1 + the largest height of a kid

    static Expr literal(Value v, SourceLoc loc = {}, bool is_char = false);
    static Expr label(std::string name, SourceLoc loc = {});
    static Expr restore(std::string name, SourceLoc loc = {});
    static Expr neg(Expr e, SourceLoc loc = {});
    static Expr binary(BinOp op, Expr l, Expr r, SourceLoc loc = {});
};

// A heap-allocated value with value semantics (copying copies the pointee); keeps rarely used or
// large parts of Word small, since a program may hold millions of cells.
template <class T> class Box {
  public:
    Box() = default;
    Box(T v) : p_(std::make_unique<T>(std::move(v))) {}
    Box(const Box& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Box(Box&&) noexcept = default;
    Box& operator=(const Box& o) {
        if (this != &o) {
            p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
        }
        return *this;
    }
    Box& operator=(Box&&) noexcept = default;
    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    const T* operator->() const { return p_.get(); }
    explicit operator bool() const noexcept { return p_ != nullptr; }

  private:
    std::unique_ptr<T> p_;
};

struct Label {
    std::string name;
    SourceLoc loc;
};

struct NameRef {
    std::string name;
    SourceLoc loc;
};

// The operands of `U_X Y`.
struct UseRef {
    NameRef x;
    NameRef y;
};

struct Word {
    enum class Kind {
        Command, // code: one command (ops.size() == 1)
        Cycle,   // code: ops.size() >= 2, Nop included
        RNop,    // code
        Expr,    // data: expr
        Unknown, // data: `?`
        Unused,  // data: `?-`
        UseRNop, // data: `U_X Y` (x = X, y = Y)
    };

    Kind kind = Kind::Command;
    SourceLoc loc;
    std::vector<Op> ops;
    Box<Expr> expr;  // Expr words
    Box<UseRef> use; // UseRNop words

    bool is_code() const noexcept {
        return kind == Kind::Command || kind == Kind::Cycle || kind == Kind::RNop;
    }
};

struct Cell {
    std::vector<Label> labels;
    Word word;
    // The cell starts a source line (the first cell of a block always does). docs/asm.md 2,
    // "Lines in data blocks": the first word of a line is a code-reference position (6), so the
    // printer and same_structure keep the grouping.
    bool starts_line = true;
};

struct Offset {
    Expr expr;
    SourceLoc loc; // the `@` or `.OFFSET`
};

struct Block {
    Section section = Section::Code;
    bool braced = false;
    SourceLoc loc; // first token of the block
    std::optional<Offset> offset;
    std::vector<Cell> cells;
};

struct Width {
    unsigned value = 0;
    SourceLoc loc; // the `.WIDTH`
};

struct Program {
    std::vector<SourceFile> files;
    std::optional<Width> width;
    std::vector<Block> blocks;
};

// Structural equality that ignores source locations, the file table, Expr::height and
// Expr::is_char.
bool same_structure(const Expr& a, const Expr& b);
bool same_structure(const Program& a, const Program& b);

} // namespace malm::assembler
