#include "asm/disasm.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "asm/check.hpp"
#include "asm/cycles.hpp"
#include "asm/layout.hpp"
#include "asm/parser.hpp"
#include "asm/printer.hpp"
#include "core/ops.hpp"
#include "core/placement.hpp"
#include "core/vm.hpp"

namespace malm::assembler {
namespace {

using Raw =
    std::uint64_t; // an address as Value::raw(); within a window, raw order is address order
using R = core::Result<Disassembly, std::string>;

std::uint32_t position(Raw a) { return Value::from_raw(a).mod94(); }

std::optional<Raw> next(Raw a) {
    const auto v = Value::from_raw(a).incremented();
    return v ? std::optional<Raw>(v->raw()) : std::nullopt;
}

bool is_char(Value v) { return v.raw() >= 33 && v.raw() <= 126; }

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

std::string ops_text(std::size_t p, std::uint8_t ch) {
    const OpSequence seq = op_sequence(p, ch);
    return cycle_text({seq.ops.begin(), seq.ops.begin() + static_cast<std::ptrdiff_t>(seq.length)});
}

// `2-cycle #1`: the length of the character's xlat2 cycle and its index in it (cycles.hpp).
std::string cycle_note(std::uint8_t ch) {
    return std::to_string(cycle_length(ch)) + "-cycle #" + std::to_string(cycle_index(ch));
}

// ---- Recognition ----

struct Recovered {
    std::map<Raw, Value> image; // ordered by raw address
    Raw entry = 0;
    unsigned width = 0;
    std::uint64_t steps = 0;
};

struct NoIo final : core::Io {
    int read_byte() override { return core::kIoError; }
    bool write_byte(std::uint8_t) override { return false; }
    bool flush() override { return true; }
};

// Runs the startup code to its final Jmp. On failure, the reason the text is not recognised.
core::Result<Recovered, std::string> recover(std::span<const std::uint8_t> cells) {
    using RR = core::Result<Recovered, std::string>;
    core::Vm vm(cells);
    NoIo io;
    std::set<Raw> written;
    for (;;) {
        if (vm.steps() > cells.size()) {
            return RR::err("no final Jmp within " + std::to_string(cells.size()) + " steps");
        }
        const Value c = vm.c();
        const Value cell = vm.memory().read(c);
        if (!is_char(cell)) {
            return RR::err("it faults at step " + std::to_string(vm.steps() + 1));
        }
        const Op op = core::decode(c.mod94(), static_cast<std::uint32_t>(cell.raw()));
        if (op == Op::Jmp && vm.steps() > 0) {
            break;
        }
        if (op == Op::In || op == Op::Out || op == Op::Hlt) {
            return RR::err("it executes " + std::string(core::op_name(op)) + " at step " +
                           std::to_string(vm.steps() + 1) + ", before a second Jmp");
        }
        const Value d = vm.d();
        if (op == Op::Opr && (d.head() != 0 || d.raw() >= cells.size())) {
            written.insert(d.raw());
        }
        if (vm.step(io).status != core::Status::Continue) {
            return RR::err("it faults at step " + std::to_string(vm.steps()));
        }
    }
    Recovered r;
    r.steps = vm.steps();
    r.width = vm.n();
    r.entry = vm.d().raw();
    for (const Raw a : written) {
        r.image.emplace(a, vm.memory().read(Value::from_raw(a)));
    }
    const auto it = r.image.find(r.entry);
    const auto land = it != r.image.end() ? it->second.incremented() : std::nullopt;
    if (!land || !r.image.contains(land->raw()) || !is_char(r.image.at(land->raw()))) {
        return RR::err("its first Jmp after cell 0 does not jump into code it wrote");
    }
    return RR::ok(std::move(r));
}

// ---- Listing ----

std::string listing(std::span<const std::uint8_t> cells, const DisasmOptions& o,
                    const std::string& why) {
    std::string out = "; malm disasm: " + o.name + ": " + std::to_string(cells.size()) +
                      " cells, " + (o.classic ? "classic" : "unleashed") + "\n; " + why +
                      ": listing the program text\n";
    out += "; address        pos  chr  sym  op    xlat2 cycle\n";
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Value a = *Value::natural(i);
        const std::uint32_t p = a.mod94();
        const std::uint8_t ch = cells[i];
        const std::uint8_t sym = core::normalize(p, ch);
        const OpSequence seq = op_sequence(p, ch);
        std::string line = pad(a.to_string(), 16);
        line += pad(std::to_string(p), 5) + pad(std::string(1, static_cast<char>(ch)), 5) +
                pad(sym == core::kNoSymbol ? "-" : std::string(1, core::kSymbolLetters[sym]), 5) +
                pad(std::string(core::op_name(seq[0])), 6) + cycle_note(ch);
        if (seq.length <= kMaxFlagLength) {
            line += ": " + ops_text(p, ch);
        }
        out += line + '\n';
    }
    return out;
}

// ---- Code words ----

// A code word that assembles to `ch` at position p, if there is one (layout.hpp, "Characters").
// Cycles of up to 9 states are written out, longer ones only if a single command will not do.
std::optional<Word> spell(std::size_t p, std::uint8_t ch) {
    const OpSequence seq = op_sequence(p, ch);
    const std::vector<Op> ops(seq.ops.begin(),
                              seq.ops.begin() + static_cast<std::ptrdiff_t>(seq.length));
    Word cycle;
    cycle.kind = Word::Kind::Cycle;
    cycle.ops = ops;
    Word command;
    command.kind = Word::Kind::Command;
    command.ops = {ops[0]};
    Word rnop;
    rnop.kind = Word::Kind::RNop;
    std::vector<const Word*> tries;
    if (is_immutable_nop(p, ch)) {
        tries = {&rnop};
    } else if (seq.length <= kMaxFlagLength) {
        tries = {&cycle, &command};
    } else {
        tries = {&command, &cycle};
    }
    for (const Word* w : tries) {
        if (word_char(*w, p) == ch) {
            return *w;
        }
    }
    return std::nullopt;
}

std::string word_text(const Word& w) {
    return w.kind == Word::Kind::RNop ? "RNop" : cycle_text(w.ops);
}

// The kind of unit a code block starts with, as a label stem: ROT, MOVED, FLAG, SLED, ...
std::string unit_name(const Word& w) {
    if (w.kind == Word::Kind::RNop) {
        return "SLED";
    }
    if (w.ops.size() > 1 && w.ops[0] == Op::Nop &&
        std::ranges::find(w.ops, Op::MovD) != w.ops.end()) {
        return "FLAG";
    }
    const auto it = std::ranges::find_if(w.ops, [](Op o) { return o != Op::Nop; });
    switch (it == w.ops.end() ? Op::Nop : *it) {
    case Op::Jmp:
        return w.ops.size() == 1 ? "NOP" : "JMP";
    case Op::Hlt:
        return "HALT";
    case Op::Rot:
        return "ROT";
    case Op::Opr:
        return "OPR";
    case Op::Out:
        return "OUT";
    case Op::In:
        return "IN";
    case Op::MovD:
        return "MOVED";
    case Op::Nop:
        break;
    }
    return "CODE";
}

// ---- Block recovery and output ----

struct CodeBlock {
    Raw start = 0; // the first cell (a U_ run included)
    Raw end = 0;   // the last cell
    std::vector<Word> words;
    bool dropped = false; // became data
};

struct DataBlock {
    Raw start = 0;
    Raw end = 0;
};

class Disassembler {
  public:
    Disassembler(std::span<const std::uint8_t> cells, const DisasmOptions& o, Recovered r)
        : cells_(cells), o_(o), r_(std::move(r)) {}

    R run() {
        if (o_.map != nullptr) {
            if (auto e = blocks_from_map(); !e.empty()) {
                return R::err(e);
            }
        } else {
            blocks_from_flow();
        }
        extend_runs();
        drop_bad_code();
        place_data();
        name_blocks();
        render_data();
        Disassembly d;
        d.recognized = true;
        d.text = emit();
        d.warning = verify(d.text);
        return R::ok(std::move(d));
    }

  private:
    std::span<const std::uint8_t> cells_;
    const DisasmOptions& o_;
    Recovered r_;
    std::vector<CodeBlock> code_;
    std::vector<DataBlock> map_data_; // the map's data blocks
    std::vector<DataBlock> data_;     // every data block of the output
    std::map<Raw, std::vector<std::string>> labels_;
    std::set<std::string> names_;
    // Per address: the code block holding it, and its reserved cell's owner.
    std::unordered_map<Raw, std::size_t> code_at_;
    std::unordered_map<Raw, std::size_t> reserved_of_;
    // Per address: the data block that owns it (its word); cells of a data block's extent owned by
    // something else are `?-` slots.
    std::unordered_map<Raw, std::size_t> data_at_;
    struct DataWord {
        std::string text;
        bool ref = false;        // a label reference: safe in a code reference position
        bool restore = false;    // `R_X`
        std::optional<Raw> code; // the code cell a jump through it lands on
    };
    std::unordered_map<Raw, DataWord> words_;

    bool in_image(Raw a) const { return r_.image.contains(a); }
    Value at(Raw a) const { return r_.image.at(a); }
    bool code_char_at(Raw a) const { return in_image(a) && is_char(at(a)); }

    void add_label(Raw a, std::string name) {
        names_.insert(name);
        labels_[a].push_back(std::move(name));
    }

    // ---- Blocks from the label map ----

    std::string blocks_from_map() {
        const std::string bad = "the label map does not match this program: ";
        std::set<std::pair<Raw, Raw>> code_seen;
        std::set<std::pair<Raw, Raw>> data_seen;
        bool entry = false;
        for (const MapEntry& e : *o_.map) {
            if (names_.contains(e.name)) {
                return bad + "label '" + e.name + "' appears twice";
            }
            add_label(e.addr.raw(), e.name);
            if (e.name == "ENTRY") {
                if (e.addr.raw() != r_.entry || e.section != Section::Data) {
                    return bad + "ENTRY is at " + Value::from_raw(r_.entry).to_string();
                }
                entry = true;
            }
            const std::pair extent{e.first.raw(), e.last.raw()};
            if (e.section == Section::Data) {
                if (data_seen.insert(extent).second) {
                    map_data_.push_back({extent.first, extent.second});
                }
                continue;
            }
            if (!code_seen.insert(extent).second) {
                continue;
            }
            for (Raw a = extent.first; a <= extent.second; ++a) {
                if (!code_char_at(a)) {
                    return bad + "the code block of '" + e.name + "' has no code at " +
                           Value::from_raw(a).to_string();
                }
            }
            code_.push_back({extent.first, extent.second, {}, false});
        }
        if (!entry) {
            return bad + "it has no ENTRY";
        }
        return {};
    }

    // ---- Blocks from the control flow reachable from ENTRY ----

    void blocks_from_flow() {
        add_label(r_.entry, "ENTRY");
        std::set<Raw> code;
        std::vector<std::pair<Raw, Raw>> work; // (C, D) before executing the cell at C
        std::set<std::pair<Raw, Raw>> seen;
        // A Jmp with D = p: C := [p] + 1, D := p + 1.
        const auto jump = [&](Raw p) {
            const auto w = r_.image.find(p);
            const auto c = w != r_.image.end() ? w->second.incremented() : std::nullopt;
            const auto d = next(p);
            if (c && d) {
                work.emplace_back(c->raw(), *d);
            }
        };
        constexpr std::size_t kMaxStates = std::size_t{1} << 22;
        jump(r_.entry);
        while (!work.empty() && seen.size() < kMaxStates) {
            const auto [c, d] = work.back();
            work.pop_back();
            if (!code_char_at(c) || !seen.emplace(c, d).second) {
                continue;
            }
            code.insert(c);
            // Every state of a flag or counter; only the first of one-shot code (docs/asm.md 3.1)
            // and of a Jmp, which its own execution never encrypts.
            const OpSequence seq = op_sequence(position(c), static_cast<std::uint8_t>(at(c).raw()));
            const auto c1 = next(c);
            const bool cycles = seq.length <= kMaxFlagLength && seq[0] != Op::Jmp;
            std::set<Op> ops(seq.ops.begin(), seq.ops.begin() + static_cast<std::ptrdiff_t>(
                                                                    cycles ? seq.length : 1));
            for (const Op op : ops) {
                if (op == Op::Jmp) {
                    jump(d);
                } else if (op == Op::MovD) {
                    // D := [D] + 1 after the increment.
                    const auto w = r_.image.find(d);
                    const auto d1 = w != r_.image.end() ? w->second.incremented() : std::nullopt;
                    if (c1 && d1) {
                        work.emplace_back(*c1, d1->raw());
                    }
                } else if (op != Op::Hlt) {
                    if (const auto d1 = next(d); c1 && d1) {
                        work.emplace_back(*c1, *d1);
                    }
                }
            }
        }
        for (auto it = code.begin(); it != code.end();) {
            CodeBlock b{*it, *it, {}, false};
            for (++it; it != code.end() && next(b.end) == *it; ++it) {
                b.end = *it;
            }
            code_.push_back(std::move(b));
        }
    }

    // ---- Code block checks ----

    void index_code() {
        code_at_.clear();
        reserved_of_.clear();
        for (std::size_t i = 0; i < code_.size(); ++i) {
            if (code_[i].dropped) {
                continue;
            }
            for (Raw a = code_[i].start; a <= code_[i].end; ++a) {
                code_at_[a] = i;
            }
            reserved_of_[code_[i].start - 1] = i;
        }
    }

    // A code block may grow backwards over RNops that some image word lands on (a `U_` run,
    // docs/asm.md 5.2), as far as a cell that can be its reserved cell.
    void extend_runs() {
        std::unordered_set<Raw> values;
        for (const auto& [a, v] : r_.image) {
            values.insert(v.raw());
        }
        std::set<Raw> taken;
        for (const CodeBlock& b : code_) {
            for (Raw a = b.start; a <= b.end; ++a) {
                taken.insert(a);
            }
        }
        // A run may lie on `?-` slots of a data block, but not on a labelled data cell.
        if (o_.map != nullptr) {
            for (const MapEntry& e : *o_.map) {
                taken.insert(e.addr.raw());
            }
        }
        for (CodeBlock& b : code_) {
            std::optional<Raw> best;
            for (Raw a = b.start - 1; a > 0 && b.start - a <= 4096; --a) {
                if (taken.contains(a) || !in_image(a) || at(a).raw() != rnop_char(position(a))) {
                    break;
                }
                if (values.contains(a - 1) && reserved_ok(a - 1)) {
                    best = a;
                }
            }
            if (best) {
                for (Raw a = *best; a < b.start; ++a) {
                    taken.insert(a);
                }
                b.start = *best;
            }
        }
    }

    // The cell before a code block holds what layout would write there: the loadable Nop, or an
    // RNop when one shares the cell (checked against the other blocks in drop_bad_code).
    bool reserved_ok(Raw a) const {
        if (!in_image(a)) {
            return false;
        }
        const std::uint32_t p = position(a);
        return at(a).raw() == loadable_char(p, Op::Nop) || at(a).raw() == rnop_char(p);
    }

    void drop_bad_code() {
        for (CodeBlock& b : code_) {
            for (Raw a = b.start; a <= b.end && !b.dropped; ++a) {
                const auto w = spell(position(a), static_cast<std::uint8_t>(at(a).raw()));
                if (w) {
                    b.words.push_back(*w);
                } else {
                    b.dropped = true;
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            index_code();
            for (std::size_t i = 0; i < code_.size(); ++i) {
                CodeBlock& b = code_[i];
                if (b.dropped) {
                    continue;
                }
                const Raw r = b.start - 1;
                bool ok = reserved_ok(r);
                if (const auto it = code_at_.find(r); ok && it != code_at_.end()) {
                    // Shared with a cell of another code block: that cell must be an RNop.
                    const CodeBlock& o = code_[it->second];
                    ok = o.words[r - o.start].kind == Word::Kind::RNop;
                } else if (ok) {
                    ok = at(r).raw() == loadable_char(position(r), Op::Nop);
                }
                if (!ok) {
                    b.dropped = true;
                    changed = true;
                }
            }
        }
        index_code();
    }

    // ---- Data blocks ----

    bool claimed_by_code(Raw a) const { return code_at_.contains(a) || reserved_of_.contains(a); }

    void place_data() {
        // The map's data blocks own their cells unless code claims them, in address order, but a
        // labelled cell belongs to its label's block.
        std::ranges::sort(map_data_, {}, &DataBlock::start);
        std::map<std::pair<Raw, Raw>, std::size_t> by_extent;
        for (std::size_t i = 0; i < map_data_.size(); ++i) {
            by_extent.emplace(std::pair{map_data_[i].start, map_data_[i].end}, i);
        }
        std::unordered_map<Raw, std::size_t> labelled;
        if (o_.map != nullptr) {
            for (const MapEntry& e : *o_.map) {
                if (e.section == Section::Data) {
                    labelled[e.addr.raw()] = by_extent.at({e.first.raw(), e.last.raw()});
                }
            }
        }
        data_ = map_data_;
        for (std::size_t i = 0; i < data_.size(); ++i) {
            for (Raw a = data_[i].start; a <= data_[i].end; ++a) {
                const auto l = labelled.find(a);
                if (!claimed_by_code(a) && (l == labelled.end() || l->second == i)) {
                    data_at_.try_emplace(a, i);
                }
            }
        }
        // Everything else in the image: runs of consecutive cells.
        std::optional<DataBlock> cur;
        const auto flush = [&] {
            if (cur) {
                data_.push_back(*cur);
                for (Raw a = cur->start; a <= cur->end; ++a) {
                    data_at_[a] = data_.size() - 1;
                }
                cur.reset();
            }
        };
        for (const auto& [a, v] : r_.image) {
            if (claimed_by_code(a) || data_at_.contains(a)) {
                flush();
                continue;
            }
            if (cur && cur->end < a && a - cur->end <= kMaxGap + 1 && free_between(cur->end, a)) {
                cur->end = a; // the cells between become `?`
            } else {
                flush();
                cur = DataBlock{a, a};
            }
        }
        flush();
        // A block may not start with a word that is unsafe as a code reference (docs/asm.md 6);
        // a free cell before it can take a `?` instead.
        for (std::size_t i = map_data_.size(); i < data_.size(); ++i) {
            DataBlock& b = data_[i];
            const Raw p = b.start - 1;
            if (!safe_ref(at(b.start)) && !in_image(p) && !claimed_by_code(p) &&
                !data_at_.contains(p) && p >= cells_.size()) {
                b.start = p;
                data_at_[p] = i;
            }
        }
    }

    // Unwritten cells between two data cells that nothing else uses are written as `?` (a `?-`
    // operand, typically), up to this many in a row.
    static constexpr Raw kMaxGap = 4;

    bool free_between(Raw lo, Raw hi) const {
        if (Value::from_raw(lo).head() != Value::from_raw(hi).head()) {
            return false;
        }
        for (Raw a = lo + 1; a < hi; ++a) {
            if (in_image(a) || claimed_by_code(a) || data_at_.contains(a)) {
                return false;
            }
        }
        return true;
    }

    // A constant word that may start a data line: not 0, landing address of width <= n - 1.
    bool safe_ref(Value v) const {
        const auto land = v.incremented();
        return v != Value() && land && land->fits_width(r_.width - 1);
    }

    // ---- Labels ----

    std::string address_name(Raw a) {
        const std::string s = Value::from_raw(a).to_string(); // 0t... or ...h...
        std::string name = s.starts_with("0t") ? "L" + s : "L" + s.substr(3, 1) + "t" + s.substr(4);
        return unique(name);
    }

    std::string unique(const std::string& stem) {
        std::string name = stem;
        for (std::size_t k = 2; names_.contains(name); ++k) {
            name = stem + "_" + std::to_string(k);
        }
        return name;
    }

    // The label naming cell a, made up if needed.
    const std::string& label_of(Raw a) {
        auto& names = labels_[a];
        if (names.empty()) {
            std::string name = address_name(a);
            names_.insert(name);
            names.push_back(std::move(name));
        }
        return names.front();
    }

    void name_blocks() {
        for (const CodeBlock& b : code_) {
            if (!b.dropped && labels_[b.start].empty()) {
                std::string name =
                    o_.map != nullptr ? address_name(b.start) : unique(unit_name(b.words.front()));
                add_label(b.start, std::move(name));
            }
        }
        for (const DataBlock& b : data_) {
            label_of(b.start);
        }
    }

    // ---- Output ----

    enum class Slot { Word, Unknown, Unused };

    Slot data_slot(std::size_t block, Raw a) const {
        const auto it = data_at_.find(a);
        if (it == data_at_.end() || it->second != block) {
            return Slot::Unused;
        }
        return in_image(a) ? Slot::Word : Slot::Unknown;
    }

    // A data word: a label reference where the value points into a block, else a constant.
    DataWord data_word(Value v) {
        const auto land = v.incremented();
        const std::optional<Raw> t = land ? std::optional<Raw>(land->raw()) : std::nullopt;
        const std::optional<Raw> code = t && code_at_.contains(*t) ? t : std::nullopt;
        if (const auto it = code_at_.find(v.raw()); it != code_at_.end()) {
            const CodeBlock& b = code_[it->second];
            const Word& w = b.words[v.raw() - b.start];
            if (w.kind == Word::Kind::Cycle && v.raw() != b.end &&
                std::ranges::any_of(w.ops, [](Op o) { return o != Op::Nop; })) {
                return {"R_" + label_of(v.raw()), true, true, code};
            }
        }
        if (t) {
            const auto d = data_at_.find(*t);
            if (code || (d != data_at_.end() && data_slot(d->second, *t) != Slot::Unused)) {
                return {label_of(*t), true, false, code};
            }
        }
        if (v.head() == 0 && v.raw() > 126) {
            const Value c = core::rotr(v, r_.width);
            if (c.raw() == '\n' || (c.raw() >= 32 && c.raw() <= 126)) {
                return {print_expr(Expr::binary(BinOp::Shl, Expr::literal(c, {}, true),
                                                Expr::literal(*Value::natural(1)))),
                        false, false, std::nullopt};
            }
        }
        return {print_expr(Expr::literal(v)), false, false, std::nullopt};
    }

    // Before any output: references may name cells of blocks printed earlier.
    void render_data() {
        for (std::size_t i = 0; i < data_.size(); ++i) {
            for (Raw a = data_[i].start; a <= data_[i].end; ++a) {
                if (data_slot(i, a) == Slot::Word) {
                    words_.emplace(a, data_word(at(a)));
                }
            }
        }
    }

    // The data words a jump landing on code cell t consumes before the unit's Jmp, judged by
    // each cell's first state; `closes` if the unit leaves the data line (MovD, Hlt) instead.
    std::size_t operands(Raw t, bool& closes) const {
        const CodeBlock& b = code_[code_at_.at(t)];
        std::size_t k = 0;
        closes = true;
        for (Raw c = t; c <= b.end; ++c) {
            switch (op_at(position(c), static_cast<std::uint8_t>(at(c).raw()), 0)) {
            case Op::Jmp:
                closes = false;
                return k;
            case Op::Hlt:
                return k;
            case Op::MovD:
                return k + 1;
            default:
                ++k;
            }
        }
        return k;
    }

    static constexpr std::size_t kLabelColumn = 16;
    static constexpr std::size_t kCommentColumn = 48;
    static constexpr std::size_t kWordsPerLine = 8;

    std::string label_text(Raw a) const {
        std::string s;
        if (const auto it = labels_.find(a); it != labels_.end()) {
            for (const std::string& n : it->second) {
                s += n + ": ";
            }
        }
        return s;
    }

    static void line(std::string& out, std::string text, const std::string& comment) {
        if (!comment.empty()) {
            text = pad(std::move(text), kCommentColumn - 1) + " ; " + comment;
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        out += text + '\n';
    }

    std::string head_line(Raw a) const {
        const std::string l = label_text(a);
        return l.size() < kLabelColumn ? pad(l, kLabelColumn) : l;
    }

    void emit_code(std::string& out, const CodeBlock& b) {
        for (Raw a = b.start; a <= b.end; ++a) {
            const std::uint32_t p = position(a);
            const auto ch = static_cast<std::uint8_t>(at(a).raw());
            std::string comment = Value::from_raw(a).to_string() + " p=" + std::to_string(p) +
                                  " '" + static_cast<char>(ch) + "' " + cycle_note(ch);
            const Word& w = b.words[a - b.start];
            if (w.kind == Word::Kind::Command && cycle_length(ch) <= kMaxFlagLength) {
                comment += ": " + ops_text(p, ch);
            }
            line(out, head_line(a) + word_text(w), comment);
        }
    }

    // Lines follow the D-driven model (docs/asm.md 1): a code reference, the operands its unit
    // consumes, then any restores, as in `ROT 'H'<<1 R_ROT`. A line may only start with a word that
    // is safe in a code reference position (docs/asm.md 6).
    void emit_data(std::string& out, std::size_t i) {
        const DataBlock& b = data_[i];
        std::string text;
        std::string comment;
        std::size_t words = 0;
        std::size_t remaining = 0; // operands of the current unit still to come
        bool open = false;         // a restore may still join the current line
        for (Raw a = b.start; a <= b.end; ++a) {
            const Slot s = data_slot(i, a);
            const DataWord* dw = s == Slot::Word ? &words_.at(a) : nullptr;
            const bool safe = dw == nullptr || dw->ref || safe_ref(at(a));
            // A `?-` slot's labels belong to the block on it.
            const bool labelled =
                s != Slot::Unused && labels_.contains(a) && !labels_.at(a).empty();
            bool fresh = false;
            if (remaining > 0) {
                --remaining;
            } else if (dw != nullptr && dw->code) {
                bool closes = false;
                remaining = operands(*dw->code, closes);
                fresh = !dw->restore || !open || labelled;
                open = !closes;
            } else {
                fresh = safe && (labelled || words >= kWordsPerLine);
                open = false;
            }
            if (a == b.start || fresh) {
                if (a != b.start) {
                    line(out, text, comment);
                }
                text = s != Slot::Unused ? head_line(a) : pad("", kLabelColumn);
                comment = Value::from_raw(a).to_string();
                words = 0;
            } else {
                text += ' ' + (s != Slot::Unused ? label_text(a) : "");
            }
            text += s == Slot::Unused ? "?-" : s == Slot::Unknown ? "?" : dw->text;
            ++words;
        }
        line(out, text, comment);
    }

    std::string emit() {
        std::size_t n_code = 0;
        std::size_t n_code_blocks = 0;
        for (const CodeBlock& b : code_) {
            if (!b.dropped) {
                n_code += b.end - b.start + 1;
                ++n_code_blocks;
            }
        }
        std::string out = "; malm disasm: " + o_.name + "\n";
        out += "; program text: " + std::to_string(cells_.size()) + " cells of startup code, " +
               std::to_string(r_.steps) + " steps to ENTRY at " +
               Value::from_raw(r_.entry).to_string() + "\n";
        out += "; image: " + std::to_string(r_.image.size()) + " cells; " +
               std::to_string(n_code_blocks) + " code blocks (" + std::to_string(n_code) +
               " cells), " + std::to_string(data_.size()) + " data blocks; " +
               (o_.map != nullptr ? "labels from the label map"
                                  : "code reached from ENTRY, labels made up") +
               "\n";
        out += ".WIDTH " + std::to_string(r_.width) + "\n";
        // Blocks in address order.
        std::vector<std::pair<Raw, std::ptrdiff_t>> order; // code: index, data: -1 - index
        for (std::size_t i = 0; i < code_.size(); ++i) {
            if (!code_[i].dropped) {
                order.emplace_back(code_[i].start, static_cast<std::ptrdiff_t>(i));
            }
        }
        for (std::size_t i = 0; i < data_.size(); ++i) {
            order.emplace_back(data_[i].start, -1 - static_cast<std::ptrdiff_t>(i));
        }
        std::ranges::sort(order);
        std::optional<Section> section;
        for (const auto& [start, k] : order) {
            const Section s = k >= 0 ? Section::Code : Section::Data;
            if (section != s) {
                out += s == Section::Code ? "\n.CODE\n" : "\n.DATA\n";
                section = s;
            }
            out += "\n@" + print_expr(Expr::literal(Value::from_raw(start))) + "\n";
            if (k >= 0) {
                emit_code(out, code_[static_cast<std::size_t>(k)]);
            } else {
                emit_data(out, static_cast<std::size_t>(-1 - k));
            }
        }
        return out;
    }

    // ---- Verification ----

    std::string verify(const std::string& text) const {
        const std::string path = "disasm.mua";
        MemoryLoader loader({{path, text}});
        Diagnostics diags;
        const Program p = analyze(path, loader, diags);
        std::optional<Layout> l;
        if (!diags.has_errors()) {
            l = layout_program(p, diags, LayoutOptions{10, cells_.size()});
        }
        if (diags.has_errors()) {
            const std::string d = format_diagnostics(diags, p.files);
            return "the output does not assemble: " + d.substr(0, d.find('\n'));
        }
        if (l->width != r_.width) {
            return "the output lays out at width " + std::to_string(l->width);
        }
        for (const ImageCell& c : l->cells) {
            const auto it = r_.image.find(c.addr.raw());
            if (it == r_.image.end() || it->second != c.value) {
                return "the output writes " + c.value.to_string() + " at " + c.addr.to_string();
            }
        }
        if (l->cells.size() != r_.image.size()) {
            return "the output writes " + std::to_string(l->cells.size()) + " cells, not " +
                   std::to_string(r_.image.size());
        }
        return {};
    }
};

} // namespace

core::Result<Disassembly, std::string> disassemble(std::span<const std::uint8_t> cells,
                                                   const DisasmOptions& options) {
    std::string why = options.classic ? "a classic program" : "listing requested";
    if (!options.classic && !options.raw) {
        auto r = recover(cells);
        if (r) {
            return Disassembler(cells, options, std::move(*r)).run();
        }
        why = "not a program from malm asm: " + r.error();
    }
    if (options.map != nullptr && !options.raw) {
        return R::err("a label map needs a program from malm asm, but this is " + why);
    }
    Disassembly d;
    d.text = listing(cells, options, why);
    return R::ok(std::move(d));
}

} // namespace malm::assembler
