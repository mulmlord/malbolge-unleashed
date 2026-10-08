#include "asm/check.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "asm/cycles.hpp"
#include "asm/parser.hpp"

namespace malm::assembler {
namespace {

struct LabelInfo {
    SourceLoc loc;
    std::size_t block = 0;
    std::size_t cell = 0;
};

std::string cycle_text(const std::vector<Op>& ops) {
    std::string s;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) {
            s += '/';
        }
        s += core::op_name(ops[i]);
    }
    return s;
}

// docs/asm.md 3.1: a cycle of written length m exists if some character c at some position p has
// an xlat2 cycle whose length divides m and whose decode sequence repeated equals the written
// one. A cycle written entirely of Nops matches every all-Nop cycle (rnop_char), so it always
// exists.
bool cycle_exists(const std::vector<Op>& ops) {
    if (std::all_of(ops.begin(), ops.end(), [](Op o) { return o == Op::Nop; })) {
        return true;
    }
    for (std::size_t id = 0; id < kNumCycles; ++id) {
        const std::size_t len = cycle_length_of_id(id);
        if (ops.size() % len != 0) {
            continue;
        }
        const std::string_view members = cycle_members(id);
        for (std::size_t p = 0; p < 94; ++p) {
            for (const char m : members) {
                const auto c = static_cast<std::uint8_t>(m);
                bool match = true;
                for (std::size_t k = 0; k < ops.size() && match; ++k) {
                    match = op_at(p, c, k) == ops[k];
                }
                if (match) {
                    return true;
                }
            }
        }
    }
    return false;
}

class Checker {
  public:
    Checker(const Program& p, Diagnostics& d) : p_(p), d_(d) {}

    void run() {
        define_labels();
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            check_block(b);
        }
        check_entry();
    }

  private:
    const Program& p_;
    Diagnostics& d_;
    std::unordered_map<std::string, LabelInfo> labels_;
    std::unordered_map<std::string, bool> cycle_cache_;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::string>> reported_;

    // The same message at the same place is reported once (a string separator is copied into
    // many cells).
    void err(SourceLoc loc, std::string msg) {
        if (reported_.emplace(loc.file, loc.line, loc.col, msg).second) {
            d_.error(loc, std::move(msg));
        }
    }

    const LabelInfo* find(const std::string& name) const {
        const auto it = labels_.find(name);
        return it == labels_.end() ? nullptr : &it->second;
    }

    const Cell& cell_of(const LabelInfo& l) const { return p_.blocks[l.block].cells[l.cell]; }
    bool is_code(const LabelInfo& l) const { return p_.blocks[l.block].section == Section::Code; }

    void define_labels() {
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            const auto& cells = p_.blocks[b].cells;
            for (std::size_t c = 0; c < cells.size(); ++c) {
                for (const Label& l : cells[c].labels) {
                    const auto [it, fresh] = labels_.try_emplace(l.name, LabelInfo{l.loc, b, c});
                    if (!fresh) {
                        d_.error(l.loc, "duplicate label '" + l.name + "'")
                            .notes.push_back(Note{it->second.loc, "first defined here"});
                    }
                }
            }
        }
    }

    void check_block(std::size_t b) {
        const Block& blk = p_.blocks[b];
        if (blk.offset) {
            check_constant(blk.offset->expr);
        }
        if (!blk.cells.empty() && blk.cells.front().labels.empty()) {
            err(blk.cells.front().word.loc, "block does not start with a label");
        }
        for (std::size_t c = 0; c < blk.cells.size(); ++c) {
            const Cell& cell = blk.cells[c];
            const Word& w = cell.word;
            if (w.kind == Word::Kind::Unused) {
                for (const Label& l : cell.labels) {
                    err(l.loc, "label '" + l.name + "' cannot name a '?-' cell");
                }
            } else if (w.kind == Word::Kind::Cycle) {
                check_cycle(w);
            } else if (w.kind == Word::Kind::Expr) {
                check_refs(*w.expr);
            } else if (w.kind == Word::Kind::UseRNop) {
                check_use_rnop(w, b, c);
            }
        }
    }

    void check_cycle(const Word& w) {
        const std::string key = cycle_text(w.ops);
        auto it = cycle_cache_.find(key);
        if (it == cycle_cache_.end()) {
            it = cycle_cache_.emplace(key, cycle_exists(w.ops)).first;
        }
        if (!it->second) {
            err(w.loc, "cycle '" + key + "' does not exist at any position");
        }
    }

    // `@` expressions: no labels, no shifts (docs/asm.md 8.2).
    void check_constant(const Expr& e) {
        switch (e.kind) {
        case Expr::Kind::Literal:
            return;
        case Expr::Kind::Label:
        case Expr::Kind::Restore:
            err(e.loc, "labels are not allowed in an offset expression");
            return;
        case Expr::Kind::Neg:
            check_constant(e.kids[0]);
            return;
        case Expr::Kind::Binary:
            if (e.op == BinOp::Shl || e.op == BinOp::Shr) {
                err(e.loc, std::string("'") + (e.op == BinOp::Shl ? "<<" : ">>") +
                               "' is not allowed in an offset expression");
            }
            check_constant(e.kids[0]);
            check_constant(e.kids[1]);
            return;
        }
    }

    void check_refs(const Expr& e) {
        switch (e.kind) {
        case Expr::Kind::Literal:
            return;
        case Expr::Kind::Label:
            if (!find(e.name)) {
                err(e.loc, "unknown label '" + e.name + "'");
            }
            return;
        case Expr::Kind::Restore:
            check_restore(e);
            return;
        case Expr::Kind::Neg:
            check_refs(e.kids[0]);
            return;
        case Expr::Kind::Binary:
            check_refs(e.kids[0]);
            check_refs(e.kids[1]);
            return;
        }
    }

    void check_restore(const Expr& e) {
        const LabelInfo* l = find(e.name);
        if (!l) {
            err(e.loc, "unknown label '" + e.name + "'");
            return;
        }
        const std::string r = "'R_" + e.name + "'";
        if (!is_code(*l)) {
            err(e.loc, r + " needs a code label, but '" + e.name + "' names a data cell");
            return;
        }
        const Word& w = cell_of(*l).word;
        if (w.kind == Word::Kind::Command) {
            err(e.loc, r + " needs a cycle cell, but '" + e.name + "' is a single command");
        } else if (w.kind == Word::Kind::RNop) {
            err(e.loc, r + " needs a cycle cell, but '" + e.name + "' is an RNop");
        } else if (w.kind == Word::Kind::Cycle &&
                   std::all_of(w.ops.begin(), w.ops.end(), [](Op o) { return o == Op::Nop; })) {
            err(e.loc, r + " needs a cycle cell, but '" + e.name + "' is an all-Nop cycle");
        } else if (l->cell + 1 >= p_.blocks[l->block].cells.size()) {
            err(e.loc, r + " needs a cell after '" + e.name + "' in its block");
        }
    }

    void check_use_rnop(const Word& w, std::size_t block, std::size_t cell) {
        const std::string u = "'U_" + w.use->x.name + "'";
        if (const LabelInfo* x = find(w.use->x.name)) {
            if (!is_code(*x)) {
                err(w.use->x.loc,
                    u + " needs a code label, but '" + w.use->x.name + "' names a data cell");
            } else if (x->cell != 0) {
                err(w.use->x.loc, u + " needs a label on the first cell of a code block, but '" +
                                      w.use->x.name + "' is not on the first cell of its block");
            }
        } else {
            err(w.use->x.loc, "unknown label '" + w.use->x.name + "'");
        }
        if (const LabelInfo* y = find(w.use->y.name)) {
            if (is_code(*y)) {
                err(w.use->y.loc, u + ": '" + w.use->y.name + "' must be a data label");
            } else if (y->block != block) {
                err(w.use->y.loc,
                    u + ": '" + w.use->y.name + "' must be defined in the same data block");
            } else if (y->cell <= cell) {
                err(w.use->y.loc,
                    u + ": '" + w.use->y.name + "' must be defined after the 'U_' word");
            }
        } else {
            err(w.use->y.loc, "unknown label '" + w.use->y.name + "'");
        }
    }

    void check_entry() {
        const LabelInfo* e = find("ENTRY");
        if (!e) {
            err(SourceLoc{0, 1, 1}, "missing ENTRY: the program must define the label ENTRY in a "
                                    ".DATA block");
            return;
        }
        if (is_code(*e)) {
            err(e->loc, "ENTRY must be defined in a .DATA block");
            return;
        }
        const Word& w = cell_of(*e).word;
        bool ok = w.kind == Word::Kind::UseRNop;
        if (w.kind == Word::Kind::Expr &&
            (w.expr->kind == Expr::Kind::Label || w.expr->kind == Expr::Kind::Restore)) {
            const LabelInfo* target = find(w.expr->name);
            if (!target) {
                return; // already reported as an unknown label
            }
            ok = is_code(*target);
        }
        if (!ok) {
            err(w.loc, "the first word of ENTRY must be a code reference (a code label, R_<code "
                       "label> or a U_ word)");
        }
    }
};

} // namespace

void check_program(const Program& program, Diagnostics& diags) { Checker(program, diags).run(); }

Program analyze(const std::string& path, FileLoader& loader, Diagnostics& diags,
                const ParseLimits& limits) {
    Program p = parse_program(path, loader, diags, limits);
    if (!diags.has_errors()) {
        check_program(p, diags);
    }
    return p;
}

} // namespace malm::assembler
