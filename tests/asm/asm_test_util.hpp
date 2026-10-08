#pragma once

// Helpers shared by the assembler front-end tests.

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "asm/ast.hpp"
#include "asm/check.hpp"
#include "asm/diagnostics.hpp"
#include "asm/parser.hpp"

namespace asmtest {

using namespace malm::assembler;

using Files = std::unordered_map<std::string, std::string>;

struct Parsed {
    Program program;
    Diagnostics diags;
    std::string text() const { return format_diagnostics(diags, program.files); }
    // The `L:C: message` of every error (notes excluded), file name stripped for the root.
    std::vector<std::string> errors() const {
        std::vector<std::string> out;
        for (const Diagnostic& d : diags.items()) {
            out.push_back(std::to_string(d.loc.line) + ":" + std::to_string(d.loc.col) + ": " +
                          d.message);
        }
        return out;
    }
};

// Parse only.
inline Parsed parse_src(std::string_view src, Files extra = {}) {
    extra["t.mua"] = std::string(src);
    MemoryLoader loader(std::move(extra));
    Parsed r;
    r.program = parse_program("t.mua", loader, r.diags);
    return r;
}

// Parse, then check when the parse was clean.
inline Parsed analyze_src(std::string_view src, Files extra = {}) {
    extra["t.mua"] = std::string(src);
    MemoryLoader loader(std::move(extra));
    Parsed r;
    r.program = analyze("t.mua", loader, r.diags);
    return r;
}

// A minimal valid program around `body` (data words of the ENTRY block).
inline std::string with_entry(std::string_view code, std::string_view data) {
    return ".CODE\n" + std::string(code) + "\n\n.DATA\nENTRY: " + std::string(data) + "\n";
}

// Prefix-notation dump of an expression, independent of the printer.
inline std::string sexp(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::Literal:
        return e.value.to_string();
    case Expr::Kind::Label:
        return e.name;
    case Expr::Kind::Restore:
        return "R_" + e.name;
    case Expr::Kind::Neg:
        return "(neg " + sexp(e.kids[0]) + ")";
    case Expr::Kind::Binary: {
        const char* op = e.op == BinOp::Add   ? "+"
                         : e.op == BinOp::Sub ? "-"
                         : e.op == BinOp::Shl ? "<<"
                         : e.op == BinOp::Shr ? ">>"
                                              : "!";
        return std::string("(") + op + " " + sexp(e.kids[0]) + " " + sexp(e.kids[1]) + ")";
    }
    }
    return "?";
}

// The words of the data cells of block `b`, as sexp strings (`?`, `?-`, `U_X Y` for the others).
inline std::vector<std::string> data_words(const Program& p, std::size_t b = 0) {
    std::vector<std::string> out;
    for (const Cell& c : p.blocks.at(b).cells) {
        switch (c.word.kind) {
        case Word::Kind::Expr:
            out.push_back(sexp(*c.word.expr));
            break;
        case Word::Kind::Unknown:
            out.push_back("?");
            break;
        case Word::Kind::Unused:
            out.push_back("?-");
            break;
        case Word::Kind::UseRNop:
            out.push_back("U_" + c.word.use->x.name + " " + c.word.use->y.name);
            break;
        default:
            out.push_back("<code>");
        }
    }
    return out;
}

} // namespace asmtest
