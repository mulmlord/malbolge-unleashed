#include "asm/ast.hpp"

#include <algorithm>
#include <utility>

namespace malm::assembler {

Expr Expr::literal(Value v, SourceLoc loc, bool is_char) {
    Expr e;
    e.kind = Kind::Literal;
    e.loc = loc;
    e.value = v;
    e.is_char = is_char;
    return e;
}

Expr Expr::label(std::string name, SourceLoc loc) {
    Expr e;
    e.kind = Kind::Label;
    e.loc = loc;
    e.name = std::move(name);
    return e;
}

Expr Expr::restore(std::string name, SourceLoc loc) {
    Expr e;
    e.kind = Kind::Restore;
    e.loc = loc;
    e.name = std::move(name);
    return e;
}

Expr Expr::neg(Expr x, SourceLoc loc) {
    Expr e;
    e.kind = Kind::Neg;
    e.loc = loc;
    e.height = x.height + 1;
    e.kids.push_back(std::move(x));
    return e;
}

Expr Expr::binary(BinOp op, Expr l, Expr r, SourceLoc loc) {
    Expr e;
    e.kind = Kind::Binary;
    e.loc = loc;
    e.op = op;
    e.height = std::max(l.height, r.height) + 1;
    e.kids.push_back(std::move(l));
    e.kids.push_back(std::move(r));
    return e;
}

bool same_structure(const Expr& a, const Expr& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case Expr::Kind::Literal:
        return a.value == b.value;
    case Expr::Kind::Label:
    case Expr::Kind::Restore:
        return a.name == b.name;
    case Expr::Kind::Neg:
        return same_structure(a.kids[0], b.kids[0]);
    case Expr::Kind::Binary:
        return a.op == b.op && same_structure(a.kids[0], b.kids[0]) &&
               same_structure(a.kids[1], b.kids[1]);
    }
    return false;
}

namespace {

bool same_word(const Word& a, const Word& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case Word::Kind::Command:
    case Word::Kind::Cycle:
        return a.ops == b.ops;
    case Word::Kind::RNop:
    case Word::Kind::Unknown:
    case Word::Kind::Unused:
        return true;
    case Word::Kind::Expr:
        return same_structure(*a.expr, *b.expr);
    case Word::Kind::UseRNop:
        return a.use->x.name == b.use->x.name && a.use->y.name == b.use->y.name;
    }
    return false;
}

bool same_cell(const Cell& a, const Cell& b) {
    if (a.labels.size() != b.labels.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.labels.size(); ++i) {
        if (a.labels[i].name != b.labels[i].name) {
            return false;
        }
    }
    return a.starts_line == b.starts_line && same_word(a.word, b.word);
}

bool same_block(const Block& a, const Block& b) {
    if (a.section != b.section || a.braced != b.braced ||
        a.offset.has_value() != b.offset.has_value() || a.cells.size() != b.cells.size()) {
        return false;
    }
    if (a.offset && !same_structure(a.offset->expr, b.offset->expr)) {
        return false;
    }
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        if (!same_cell(a.cells[i], b.cells[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool same_structure(const Program& a, const Program& b) {
    if (a.width.has_value() != b.width.has_value() ||
        (a.width && a.width->value != b.width->value) || a.blocks.size() != b.blocks.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.blocks.size(); ++i) {
        if (!same_block(a.blocks[i], b.blocks[i])) {
            return false;
        }
    }
    return true;
}

} // namespace malm::assembler
