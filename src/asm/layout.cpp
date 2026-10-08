#include "asm/layout.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <iterator>
#include <map>

#include "asm/cycles.hpp"
#include "asm/eval.hpp"
#include "core/placement.hpp"

namespace malm::assembler {

const ImageCell* Layout::find(Value addr) const {
    const auto it =
        std::lower_bound(cells.begin(), cells.end(), addr.raw(),
                         [](const ImageCell& c, std::uint64_t raw) { return c.addr.raw() < raw; });
    return it != cells.end() && it->addr == addr ? &*it : nullptr;
}

namespace {

using core::pow3;
using Raw =
    std::uint64_t; // an address as Value::raw(); within a window, raw order is address order
using Residues = std::bitset<94>;
using CharTable = std::array<std::uint8_t, 94>; // by position; 0 where the word does not exist

bool all_nop(const std::vector<Op>& ops) {
    return std::all_of(ops.begin(), ops.end(), [](Op o) { return o == Op::Nop; });
}

bool is_soft(const Word& w) {
    return w.kind == Word::Kind::RNop || (w.kind == Word::Kind::Cycle && all_nop(w.ops));
}

bool loadable_at(std::size_t p, std::uint8_t c) { return core::normalize(p, c) != core::kNoSymbol; }

// The character of a code word at every position (docs/asm.md 3.1; the rule is in layout.hpp).
CharTable char_table(const Word& w) {
    CharTable t{};
    for (std::size_t p = 0; p < 94; ++p) {
        if (is_soft(w)) {
            t[p] = rnop_char(p);
        } else if (w.kind == Word::Kind::Command) {
            t[p] = loadable_char(p, w.ops[0]);
        } else {
            for (unsigned c = 33; c <= 126 && (t[p] == 0 || !loadable_at(p, t[p])); ++c) {
                const auto ch = static_cast<std::uint8_t>(c);
                if (w.ops.size() % cycle_length(ch) != 0) {
                    continue;
                }
                bool match = true;
                for (std::size_t k = 0; k < w.ops.size() && match; ++k) {
                    match = op_at(p, ch, k) == w.ops[k];
                }
                if (match && (t[p] == 0 || loadable_at(p, ch))) {
                    t[p] = ch;
                }
            }
        }
    }
    return t;
}

std::uint8_t reserved_char(std::size_t p) { return loadable_char(p, Op::Nop); }

std::uint32_t position(Raw a) { return Value::from_raw(a).mod94(); }

// A run of footprint cells, relative to the footprint start.
struct Segment {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool soft = false;
};

struct Shape {
    std::vector<Segment> segments;
    std::uint64_t length = 0; // footprint length
    std::uint64_t lead = 0;   // offset of the block's first cell
    Residues residues;        // positions of the footprint start where every code cell exists
    std::string key;          // equal keys: equal shapes (see Placer::place_free)

    void add(bool soft, std::uint64_t n = 1) {
        if (!segments.empty() && segments.back().soft == soft &&
            segments.back().offset + segments.back().length == length) {
            segments.back().length += n;
        } else {
            segments.push_back({length, n, soft});
        }
        length += n;
    }
    void skip() { ++length; } // a `?-` slot
};

// Addresses [lo, hi) of a window (docs/asm.md 7) at program width n: the addresses of width <= n -
// 1 with that head, without the last one (its successor has width n) and, in head 0, below `base`.
struct Window {
    Raw lo = 0;
    Raw hi = 0;
};

Window window_of(unsigned head, unsigned n, Raw base) {
    const Raw size = pow3[n - 1];
    if (head == 0) {
        return {base, size - 1};
    }
    const Raw all_ones = (pow3[core::kTrits] - 1) / 2;
    const Raw low_ones = (size - 1) / 2;
    return {all_ones - low_ones, all_ones - low_ones + size - 1};
}

struct Occupant {
    Raw end = 0;
    bool soft = false;
    std::size_t block = 0;
};

// Which addresses are taken, by whom. Hard runs are stored whole, soft cells one by one.
class Occupancy {
  public:
    // The first occupant in [lo, hi) that conflicts with a cell of the given softness.
    const std::pair<const Raw, Occupant>* conflict(Raw lo, Raw hi, bool soft) const {
        auto it = m_.upper_bound(lo);
        if (it != m_.begin() && std::prev(it)->second.end > lo) {
            --it;
        }
        for (; it != m_.end() && it->first < hi; ++it) {
            if (!(soft && it->second.soft)) {
                return &*it;
            }
        }
        return nullptr;
    }

    // Precondition: no conflict.
    void insert(Raw lo, Raw hi, bool soft, std::size_t block) {
        if (!soft) {
            m_.emplace(lo, Occupant{hi, false, block});
            return;
        }
        for (Raw a = lo; a < hi; ++a) {
            m_.try_emplace(a, Occupant{a + 1, true, block});
        }
    }

  private:
    std::map<Raw, Occupant> m_;
};

enum class Outcome { Ok, Wider, Failed };

class Placer {
  public:
    Placer(const Program& p, Diagnostics& d, const LayoutOptions& o) : p_(p), d_(d), base_(o.base) {
        min_width_ = std::max(10u, o.min_width);
    }

    std::optional<Layout> run() {
        if (!prepare()) {
            return std::nullopt;
        }
        unsigned n = min_width_;
        if (p_.width) {
            n = p_.width->value;
        } else {
            const auto top = Value::natural(base_ + hard_cells_);
            n = std::max(n, top ? top->width() + 1 : core::kMaxWidth);
        }
        for (;; ++n) {
            const Outcome o = attempt(n);
            if (o == Outcome::Failed) {
                return std::nullopt;
            }
            if (o == Outcome::Ok) {
                break;
            }
            if (p_.width || n == core::kMaxWidth) {
                Diagnostic& e = d_.error(wider_loc_, wider_msg_);
                if (p_.width) {
                    e.notes.push_back(Note{p_.width->loc, "program width set here"});
                }
                return std::nullopt;
            }
        }
        return build(n);
    }

  private:
    const Program& p_;
    Diagnostics& d_;
    Raw base_ = kFirstUserAddress;
    unsigned min_width_ = 10;
    std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> label_cells_;
    std::vector<std::uint64_t> k_max_;                 // per block: its U_ RNop run
    std::vector<Shape> shapes_;                        // per block
    std::vector<std::optional<Value>> pins_;           // per block: the pinned first cell
    std::vector<std::vector<const CharTable*>> chars_; // per block and cell (code blocks)
    std::map<std::string, CharTable> char_tables_;     // by cycle text, or "RNop"
    std::uint64_t hard_cells_ = 0;                     // of unpinned blocks: a lower bound on L
    std::vector<Raw> starts_;                          // per block: the footprint start
    SourceLoc wider_loc_;
    std::string wider_msg_;

    // ---- Independent of n ----

    bool prepare() {
        const std::size_t nb = p_.blocks.size();
        k_max_.assign(nb, 0);
        shapes_.resize(nb);
        pins_.resize(nb);
        chars_.resize(nb);
        for (std::size_t b = 0; b < nb; ++b) {
            const auto& cells = p_.blocks[b].cells;
            for (std::size_t c = 0; c < cells.size(); ++c) {
                for (const Label& l : cells[c].labels) {
                    label_cells_.emplace(l.name, std::pair{b, c});
                }
            }
        }
        for (const Block& blk : p_.blocks) {
            for (std::size_t c = 0; c < blk.cells.size(); ++c) {
                const Word& w = blk.cells[c].word;
                if (w.kind == Word::Kind::UseRNop) {
                    const std::size_t x = label_cells_.at(w.use->x.name).first;
                    const std::size_t y = label_cells_.at(w.use->y.name).second;
                    k_max_[x] = std::max<std::uint64_t>(k_max_[x], y - c - 1);
                }
            }
        }
        bool ok = true;
        for (std::size_t b = 0; b < nb; ++b) {
            ok = make_shape(b) && ok;
            if (const auto& off = p_.blocks[b].offset) {
                pins_[b] = evaluate(off->expr, core::kMaxWidth, no_labels, d_);
                ok = pins_[b].has_value() && ok;
            }
        }
        return ok;
    }

    static Value no_labels(const std::string&) { return Value(); } // offsets have none (check.hpp)

    bool make_shape(std::size_t b) {
        const Block& blk = p_.blocks[b];
        Shape& s = shapes_[b];
        s.residues.set();
        if (blk.section == Section::Data) {
            for (const Cell& c : blk.cells) {
                if (c.word.kind == Word::Kind::Unused) {
                    s.skip();
                } else {
                    s.add(false);
                }
            }
        } else {
            s.add(true); // reserved cell
            if (k_max_[b] > 0) {
                s.add(true, k_max_[b]);
            }
            s.lead = s.length;
            for (std::size_t i = 0; i < blk.cells.size(); ++i) {
                const Word& w = blk.cells[i].word;
                const std::string key = is_soft(w) ? "RNop" : cycle_text(w.ops);
                auto it = char_tables_.find(key);
                if (it == char_tables_.end()) {
                    it = char_tables_.emplace(key, char_table(w)).first;
                }
                chars_[b].push_back(&it->second);
                s.add(is_soft(w));
                for (std::size_t p = 0; p < 94; ++p) {
                    if (it->second[(p + s.lead + i) % 94] == 0) {
                        s.residues.reset(p);
                    }
                }
            }
            if (s.residues.none()) {
                d_.error(blk.loc, "no position fits every cycle of this code block: its cycles "
                                  "exist only at positions that do not line up");
                return false;
            }
        }
        if (!blk.offset) {
            for (const Segment& g : s.segments) {
                hard_cells_ += g.soft ? 0 : g.length;
            }
        }
        s.key = s.residues.to_string() + ":" + std::to_string(s.lead);
        for (const Segment& g : s.segments) {
            s.key +=
                ":" + std::to_string(g.offset) + (g.soft ? "s" : "h") + std::to_string(g.length);
        }
        return true;
    }

    // ---- One placement at width n ----

    Outcome attempt(unsigned n) {
        Occupancy occ;
        starts_.assign(p_.blocks.size(), 0);
        bool failed = false;
        bool wider = false;
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            if (pins_[b]) {
                const Outcome o = place_pinned(b, n, occ);
                failed = failed || o == Outcome::Failed;
                wider = wider || o == Outcome::Wider;
            }
        }
        if (failed) {
            return Outcome::Failed;
        }
        if (wider) {
            return Outcome::Wider;
        }
        std::unordered_map<std::string, Raw> next_try; // by shape key
        const Window w = window_of(0, n, base_);
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            if (pins_[b]) {
                continue;
            }
            const Shape& s = shapes_[b];
            Raw& from = next_try.try_emplace(s.key, w.lo).first->second;
            const auto a = first_fit(s, w, from, occ);
            if (!a) {
                wider_loc_ = p_.blocks[b].loc;
                wider_msg_ = "this block does not fit in memory at program width " +
                             std::to_string(n) + " (addresses below 3^" + std::to_string(n - 1) +
                             ")";
                return Outcome::Wider;
            }
            // Occupancy only grows, so a block of the same shape cannot fit below this one.
            from = *a + 1;
            claim(b, *a, occ);
        }
        return Outcome::Ok;
    }

    std::string where(std::size_t b) const { return "block pinned at " + pins_[b]->to_string(); }

    Outcome place_pinned(std::size_t b, unsigned n, Occupancy& occ) {
        const Block& blk = p_.blocks[b];
        const Shape& s = shapes_[b];
        const Value first = *pins_[b];
        const SourceLoc loc = blk.offset->loc;
        const auto start = subtract(first, *Value::natural(s.lead));
        const auto end = start ? add(*start, *Value::natural(s.length - 1)) : std::nullopt;
        const unsigned h = first.head();
        if (h == 2) {
            d_.error(loc, where(b) + " has a head-2 address, which C and D never reach: they only "
                                     "take values loaded from memory, and no cell holds a head-2 "
                                     "value unless In returned EOF");
            return Outcome::Failed;
        }
        if (h == 0 && (!start || start->head() != 0 || start->raw() < base_)) {
            d_.error(loc,
                     where(b) + " overlaps the startup area, cells 0.." +
                         std::to_string(base_ - 1) +
                         (s.lead > 0 ? " (with its reserved cell" +
                                           std::string(k_max_[b] > 0 ? " and RNop run" : "") + ")"
                                     : ""));
            return Outcome::Failed;
        }
        if (!start || !end || start->head() != h || end->head() != h) {
            d_.error(loc, where(b) + " crosses the end of its address window");
            return Outcome::Failed;
        }
        // Writing the last cell leaves D on the cell after it, which must stay addressable too.
        const auto after = add(*end, *Value::natural(1));
        const unsigned need = std::max(start->width(), after ? after->width() : core::kMaxWidth);
        if (need > n - 1) {
            wider_loc_ = loc;
            wider_msg_ = where(b) + " needs addresses of width " + std::to_string(need) +
                         ", but program width " + std::to_string(n) + " allows at most " +
                         std::to_string(n - 1);
            return Outcome::Wider;
        }
        const Raw a = start->raw();
        if (!s.residues.test(position(a))) {
            report_position(b, a);
            return Outcome::Failed;
        }
        for (const Segment& g : s.segments) {
            if (const auto* o = occ.conflict(a + g.offset, a + g.offset + g.length, g.soft)) {
                d_.error(loc, where(b) + " overlaps another block")
                    .notes.push_back(Note{p_.blocks[o->second.block].loc, "the other block"});
                return Outcome::Failed;
            }
        }
        claim(b, a, occ);
        return Outcome::Ok;
    }

    void report_position(std::size_t b, Raw a) {
        const Block& blk = p_.blocks[b];
        const std::uint64_t lead = shapes_[b].lead;
        for (std::size_t i = 0; i < blk.cells.size(); ++i) {
            const std::uint32_t p = position(a + lead + i);
            if ((*chars_[b][i])[p] == 0) {
                d_.error(blk.cells[i].word.loc, "cycle '" + cycle_text(blk.cells[i].word.ops) +
                                                    "' does not exist at position " +
                                                    std::to_string(p) + " (" + where(b) + ")");
                return;
            }
        }
    }

    std::optional<Raw> first_fit(const Shape& s, const Window& w, Raw a,
                                 const Occupancy& occ) const {
        for (;;) {
            if (!s.residues.all()) {
                const std::size_t r = a % 94; // head 0: the position is the address mod 94
                std::size_t d = 0;
                while (!s.residues.test((r + d) % 94)) {
                    ++d;
                }
                a += d;
            }
            if (a > w.hi || w.hi - a < s.length) {
                return std::nullopt;
            }
            std::optional<Raw> next;
            for (const Segment& g : s.segments) {
                if (const auto* o = occ.conflict(a + g.offset, a + g.offset + g.length, g.soft)) {
                    next = o->second.end - g.offset;
                    break;
                }
            }
            if (!next) {
                return a;
            }
            a = *next;
        }
    }

    void claim(std::size_t b, Raw a, Occupancy& occ) {
        starts_[b] = a;
        for (const Segment& g : shapes_[b].segments) {
            occ.insert(a + g.offset, a + g.offset + g.length, g.soft, b);
        }
    }

    // ---- The image at the final width ----

    Value cell_addr(std::size_t b, std::size_t c) const {
        return Value::from_raw(starts_[b] + shapes_[b].lead + c);
    }

    std::optional<Layout> build(unsigned n) {
        Layout l;
        l.width = n;
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            const Block& blk = p_.blocks[b];
            l.blocks.push_back({cell_addr(b, 0), cell_addr(b, blk.cells.size() - 1)});
            for (std::size_t c = 0; c < blk.cells.size(); ++c) {
                for (const Label& lab : blk.cells[c].labels) {
                    l.labels.emplace(lab.name, LabelInfo{cell_addr(b, c), blk.section, b});
                }
            }
        }
        const LabelAddress address = [&l](const std::string& name) {
            return l.labels.at(name).addr;
        };
        std::map<Raw, ImageCell> image;
        const auto put = [&image](Raw a, Value v, CellRole role) {
            const std::uint32_t p = position(a);
            const bool loadable = v.raw() >= 33 && v.raw() <= 126 &&
                                  loadable_at(p, static_cast<std::uint8_t>(v.raw()));
            auto [it, fresh] =
                image.try_emplace(a, ImageCell{Value::from_raw(a), v, role, loadable});
            if (!fresh && it->second.role == CellRole::Reserved) {
                it->second = ImageCell{Value::from_raw(a), v, role, loadable}; // an RNop wins
            }
        };
        const auto chr = [](std::uint8_t c) { return *Value::natural(c); };
        bool ok = true;
        for (std::size_t b = 0; b < p_.blocks.size(); ++b) {
            const Block& blk = p_.blocks[b];
            const Raw s = starts_[b];
            if (blk.section == Section::Code) {
                put(s, chr(reserved_char(position(s))), CellRole::Reserved);
                for (Raw a = s + 1; a < s + shapes_[b].lead; ++a) {
                    put(a, chr(rnop_char(position(a))), CellRole::RNopRun);
                }
                for (std::size_t c = 0; c < blk.cells.size(); ++c) {
                    const Raw a = cell_addr(b, c).raw();
                    put(a, chr((*chars_[b][c])[position(a)]), CellRole::Code);
                }
                continue;
            }
            for (std::size_t c = 0; c < blk.cells.size(); ++c) {
                const auto v = data_value(b, c, n, address);
                if (v) {
                    put(cell_addr(b, c).raw(), *v, CellRole::Data);
                }
                ok = ok && (v || !has_value(blk.cells[c].word));
            }
        }
        if (!ok) {
            return std::nullopt;
        }
        l.cells.reserve(image.size());
        for (auto& [a, cell] : image) {
            l.cells.push_back(cell);
        }
        return l;
    }

    static bool has_value(const Word& w) {
        return w.kind == Word::Kind::Expr || w.kind == Word::Kind::UseRNop;
    }

    // A data word's stored value with the checks of docs/asm.md 4.3 and 6; nullopt for `?`, `?-`
    // and on error.
    std::optional<Value> data_value(std::size_t b, std::size_t c, unsigned n,
                                    const LabelAddress& address) {
        const Cell& cell = p_.blocks[b].cells[c];
        const Word& w = cell.word;
        if (w.kind == Word::Kind::UseRNop) {
            const auto [xb, xc] = label_cells_.at(w.use->x.name);
            const std::uint64_t k = label_cells_.at(w.use->y.name).second - c - 1;
            return subtract(cell_addr(xb, xc), *Value::natural(k + 1));
        }
        if (w.kind != Word::Kind::Expr) {
            return std::nullopt;
        }
        const auto v = evaluate(*w.expr, n, address, d_);
        if (!v) {
            return std::nullopt;
        }
        const std::string ns = " (n = " + std::to_string(n) + ")";
        if (v->head() == 2) {
            d_.error(w.loc, "the value " + v->to_string() +
                                " has head 2, which the startup code cannot build: crz and "
                                "rotation never turn heads 0 and 1 into head 2 (docs/asm.md 4.3)");
            return std::nullopt;
        }
        if (!v->fits_width(n)) {
            d_.error(w.loc, "the value " + v->to_string() + " has width " +
                                std::to_string(v->width()) + ", wider than the program width" + ns);
            return std::nullopt;
        }
        if (cell.starts_line) {
            // A code-reference position (docs/asm.md 6): jumping through 0 would encrypt cell 0,
            // and the landing address must stay addressable.
            const auto landing = add(*v, *Value::natural(1));
            if (*v == Value()) {
                d_.error(w.loc, "the first word of a data line is a code reference and must not "
                                "be 0 (jumping through it would encrypt cell 0, the startup Jmp)");
                return std::nullopt;
            }
            if (!landing || !landing->fits_width(n - 1)) {
                d_.error(w.loc, "the first word of a data line is a code reference, but its "
                                "landing address " +
                                    (landing ? landing->to_string() : v->to_string() + " + 1") +
                                    " is wider than n - 1" + ns);
                return std::nullopt;
            }
        }
        return v;
    }
};

} // namespace

std::optional<Layout> layout_program(const Program& program, Diagnostics& diags,
                                     const LayoutOptions& options) {
    return Placer(program, diags, options).run();
}

} // namespace malm::assembler
