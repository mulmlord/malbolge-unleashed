#include "asm/printer.hpp"

#include <cstdio>

namespace malm::assembler {
namespace {

constexpr int kPrecAdd = 1;
constexpr int kPrecShift = 2;
constexpr int kPrecCrz = 3;
constexpr int kPrecUnary = 4;

int precedence(BinOp op) {
    switch (op) {
    case BinOp::Add:
    case BinOp::Sub:
        return kPrecAdd;
    case BinOp::Shl:
    case BinOp::Shr:
        return kPrecShift;
    case BinOp::Crz:
        return kPrecCrz;
    }
    return kPrecAdd;
}

const char* spelling(BinOp op) {
    switch (op) {
    case BinOp::Add:
        return "+";
    case BinOp::Sub:
        return "-";
    case BinOp::Shl:
        return "<<";
    case BinOp::Shr:
        return ">>";
    case BinOp::Crz:
        return "!";
    }
    return "?";
}

std::string char_literal(std::uint64_t code) {
    std::string out = "'";
    switch (code) {
    case '\n':
        out += "\\n";
        break;
    case '\r':
        out += "\\r";
        break;
    case '\t':
        out += "\\t";
        break;
    case 0:
        out += "\\0";
        break;
    case '\\':
        out += "\\\\";
        break;
    case '\'':
        out += "\\'";
        break;
    default:
        if (code >= 32 && code < 127) {
            out += static_cast<char>(code);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(code));
            out += buf;
        }
    }
    return out + "'";
}

std::string print_prec(const Expr& e, int min_prec) {
    switch (e.kind) {
    case Expr::Kind::Literal:
        if (e.is_char && e.value.head() == 0 && e.value.raw() < 256) {
            return char_literal(e.value.raw());
        }
        if (e.value.head() == 0) {
            return std::to_string(e.value.raw());
        }
        return e.value.to_string();
    case Expr::Kind::Label:
        return e.name;
    case Expr::Kind::Restore:
        return "R_" + e.name;
    case Expr::Kind::Neg:
        return "-" + print_prec(e.kids[0], kPrecUnary);
    case Expr::Kind::Binary: {
        const int p = precedence(e.op);
        std::string s = print_prec(e.kids[0], p) + spelling(e.op) + print_prec(e.kids[1], p + 1);
        return p < min_prec ? "(" + s + ")" : s;
    }
    }
    return {};
}

std::string print_word(const Word& w) {
    switch (w.kind) {
    case Word::Kind::Command:
    case Word::Kind::Cycle: {
        std::string s;
        for (std::size_t i = 0; i < w.ops.size(); ++i) {
            if (i > 0) {
                s += '/';
            }
            s += core::op_name(w.ops[i]);
        }
        return s;
    }
    case Word::Kind::RNop:
        return "RNop";
    case Word::Kind::Unknown:
        return "?";
    case Word::Kind::Unused:
        return "?-";
    case Word::Kind::UseRNop:
        return "U_" + w.use->x.name + " " + w.use->y.name;
    case Word::Kind::Expr: {
        std::string s = print_prec(*w.expr, 0);
        return s.starts_with('-') ? "(" + s + ")" : s;
    }
    }
    return {};
}

} // namespace

std::string print_expr(const Expr& e) { return print_prec(e, 0); }

std::string print_program(const Program& p) {
    std::string out;
    if (p.width) {
        out += ".WIDTH " + std::to_string(p.width->value) + "\n";
    }
    bool first = true;
    std::optional<Section> prev;
    for (const Block& b : p.blocks) {
        if (!first || p.width) {
            out += '\n';
        }
        first = false;
        if (prev != b.section) {
            out += b.section == Section::Code ? ".CODE\n" : ".DATA\n";
            prev = b.section;
        }
        if (b.braced) {
            out += "{\n";
        }
        if (b.offset) {
            out += "@" + print_expr(b.offset->expr) + "\n";
        }
        // One source line per run of cells (Cell::starts_line); labels of the first cell of a
        // line are padded to a column, mid-line labels follow a single space.
        bool line_open = false;
        for (const Cell& c : b.cells) {
            std::string labels;
            for (const Label& l : c.labels) {
                labels += l.name + ": ";
            }
            if (c.starts_line || !line_open) {
                if (line_open) {
                    out += '\n';
                }
                if (labels.empty()) {
                    labels = std::string(8, ' ');
                } else {
                    while (labels.size() < 8) {
                        labels += ' ';
                    }
                }
                out += labels;
                line_open = true;
            } else {
                out += ' ' + labels;
            }
            out += print_word(c.word);
        }
        if (line_open) {
            out += '\n';
        }
        if (b.braced) {
            out += "}\n";
        }
    }
    return out;
}

} // namespace malm::assembler
