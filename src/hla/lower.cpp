#include "hla/lower.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asm/check.hpp"
#include "hla/word.hpp"

namespace malm::hla {
namespace {

namespace as = assembler;
using as::Cell;
using as::Expr;
using CoreOp = core::Op;

// Names (docs/hla.md 4.1). User names get a prefix, so they never clash with generated names
// (which start with `_`), with the assembler's reserved words or with its `R_`/`U_` prefixes.
std::string label_name(const std::string& n) { return "L_" + n; }
std::string reg_name(const std::string& n) { return "V_" + n; }

// Code units.
const std::string kRot = "_ROT";
const std::string kOpr = "_OPR";
const std::string kMoved = "_MOVED";
const std::string kIn = "_INP";
const std::string kOut = "_OUTP";
const std::string kHalt = "_HALT";
const std::string kMd = "_MD"; // MovD through a dispatch register
const std::string kGo = "_GO"; // MovD through the next word: a goto out of a table

// Lane-table tags (word.hpp): bit i is the trit i of the index.
constexpr unsigned kByteTag = 0;
constexpr unsigned zero_tag(unsigned lane) { return 8 | (lane & 1) << 1 | (lane >> 1) << 2; }
// Cells a table entry may span before it reaches the next index of the same window.
constexpr std::size_t kByteEntryCells = 26;

// Data words at one source location.
struct Words {
    SourceLoc loc;

    Cell word(Expr e) const {
        Cell c;
        c.word.kind = as::Word::Kind::Expr;
        c.word.loc = loc;
        c.word.expr = as::Box<Expr>(std::move(e));
        c.starts_line = false;
        return c;
    }
    Cell ref(const std::string& name) const { return word(Expr::label(name, loc)); }
    Cell restore(const std::string& name) const { return word(Expr::restore(name, loc)); }
    Cell lit(Value v) const { return word(Expr::literal(v, loc)); }
    Cell unused() const {
        Cell c;
        c.word.kind = as::Word::Kind::Unused;
        c.word.loc = loc;
        c.starts_line = false;
        return c;
    }
    Cell use(const std::string& x, const std::string& y) const {
        Cell c;
        c.word.kind = as::Word::Kind::UseRNop;
        c.word.loc = loc;
        c.word.use = as::Box<as::UseRef>(as::UseRef{{x, loc}, {y, loc}});
        c.starts_line = false;
        return c;
    }
};

Cell labeled(Cell c, const std::string& name) {
    c.labels.push_back(as::Label{name, c.word.loc});
    return c;
}

// A line of data words. A call line (callee >= 0) is preceded by the flags its caller sets, which
// are known only when every call site is (finish()).
struct Line {
    std::vector<as::Label> labels;
    std::vector<Cell> cells;
    int callee = -1;
    std::size_t site = 0;
};

struct Stream {
    std::vector<Line> lines;
    std::vector<as::Label> pending;

    void label(const std::string& name, SourceLoc loc) { pending.push_back({name, loc}); }
    void line(std::vector<Cell> cells, int callee = -1, std::size_t site = 0) {
        lines.push_back(Line{std::move(pending), std::move(cells), callee, site});
        pending.clear();
    }
};

// A routine or register that returns to its call sites through a tree of flags (docs/hla.md 4.3).
// Every exit has its own tree over the same call sites.
struct Callee {
    bool sub = false; // subroutine flags (_SF), else register-access flags (_AF)
    SourceLoc loc;    // where its support code is attributed
    std::vector<std::vector<std::string>> exits; // exits[e][site]: where the site continues
};

// The depths at which `site` lies in the upper half of the tree over `m` sites: the flags its
// caller sets.
std::vector<unsigned> site_flags(std::size_t m, std::size_t site) {
    std::vector<unsigned> out;
    std::size_t lo = 0;
    std::size_t hi = m;
    for (unsigned depth = 0; hi - lo > 1; ++depth) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (site >= mid) {
            out.push_back(depth);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return out;
}

std::string flag_name(bool sub, unsigned depth) {
    return (sub ? "_SF" : "_AF") + std::to_string(depth);
}

struct Reg {
    std::string name; // the value cell's label
    Value init;
    bool dispatch = false; // also a `U_MD` entry for MovD through it
    int callee = -1;
};

class Lowerer {
  public:
    explicit Lowerer(const Program& p) : p_(p) {}

    as::Program run() {
        out_.files = p_.files;
        out_.width = as::Width{kWidth, SourceLoc{0, 1, 1}};
        for (const RegDecl& r : p_.regs) {
            add_reg(reg_name(r.name), word(r.init), false, r.loc);
        }
        main_.label("ENTRY", p_.stmts.empty() ? SourceLoc{0, 1, 1} : p_.stmts.front().loc);
        bool falls = true;
        for (std::size_t i = 0; i < p_.stmts.size();) {
            if (p_.stmts[i].op == Op::Label) {
                const Stmt& first = p_.stmts[i];
                if (falls) {
                    const Words w{first.loc};
                    main_.line({w.ref(kMoved), w.ref(label_name(first.name))});
                }
                for (; i < p_.stmts.size() && p_.stmts[i].op == Op::Label; ++i) {
                    main_.label(label_name(p_.stmts[i].name), p_.stmts[i].loc);
                }
                main_.line({Words{first.loc}.restore(kMoved)});
                use(kMoved);
                falls = true;
                continue;
            }
            falls = statement(p_.stmts[i]);
            ++i;
        }
        if (falls) {
            const SourceLoc end = p_.stmts.empty() ? SourceLoc{0, 1, 1} : p_.stmts.back().loc;
            load(main_, Value{}, end);
            main_.line({Words{end}.ref(kHalt)});
            use(kHalt);
        }
        finish();
        return std::move(out_);
    }

  private:
    // Statements (docs/hla.md 3, 4.2). Returns whether execution can continue with the next one.
    bool statement(const Stmt& s) {
        const Words w{s.loc};
        const auto reg_of = [&](const Operand& o) { return reg_index(reg_name(o.name)); };
        switch (s.op) {
        case Op::Mov: {
            const int dst = reg_of(s.args[0]);
            if (s.args[1].kind == Operand::Kind::Imm) {
                reset(main_, dst, s.loc);
                load(main_, *crz_operand(make_value({}, 1), word(imm_word(s.args[1].imm))), s.loc);
                access(main_, dst, s.loc);
            } else if (const int src = reg_of(s.args[1]); src != dst) {
                // dst := C1; A := src; dst := crz(src, C1) (0 -> 1, 2 -> 2); with A = C1 the 1s
                // become 0.
                reset(main_, dst, s.loc);
                access(main_, src, s.loc);
                access(main_, dst, s.loc);
                load(main_, make_value({}, 1), s.loc);
                access(main_, dst, s.loc);
            }
            return true;
        }
        case Op::Jmp:
            main_.line({w.ref(kMoved), w.ref(label_name(s.args[0].name))});
            use(kMoved);
            return false;
        case Op::Bz:
        case Op::Bnz:
            zero_test(s.op == Op::Bz, reg_of(s.args[0]), label_name(s.args[1].name), s.loc);
            return true;
        case Op::In:
            input(reg_of(s.args[0]),
                  s.args.size() > 1 ? std::optional(label_name(s.args[1].name)) : std::nullopt,
                  s.loc);
            return true;
        case Op::Out:
            if (s.args[0].kind == Operand::Kind::Name) {
                to_byte(reg_of(s.args[0]), s.loc);
                main_.line({w.ref(kOut), w.unused(), w.restore(kOut)});
            } else if (s.args[0].kind == Operand::Kind::String) {
                for (const char c : s.args[0].text) {
                    load(main_, *Value::natural(static_cast<unsigned char>(c)), s.loc);
                    main_.line({w.ref(kOut), w.unused(), w.restore(kOut)});
                }
            } else {
                load(main_, *Value::natural(static_cast<std::uint64_t>(s.args[0].imm)), s.loc);
                main_.line({w.ref(kOut), w.unused(), w.restore(kOut)});
            }
            use(kOut);
            return true;
        case Op::Halt:
            if (s.args.empty()) {
                load(main_, Value{}, s.loc);
            } else if (s.args[0].kind == Operand::Kind::Name) {
                to_byte(reg_of(s.args[0]), s.loc);
            } else {
                load(main_, *Value::natural(static_cast<std::uint64_t>(s.args[0].imm)), s.loc);
            }
            main_.line({w.ref(kHalt)});
            use(kHalt);
            return false;
        case Op::Label:
            break;
        }
        return true;
    }

    // `bz`/`bnz`: one lane at a time, the lane's zero table says zero (go on with the next lane, or
    // the zero target after the last) or nonzero.
    void zero_test(bool bz, int reg, const std::string& target, SourceLoc loc) {
        const Words w{loc};
        const std::string after = fresh();
        const std::string& zero = bz ? target : after;
        const std::string& nonzero = bz ? after : target;
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            lane_index(main_, reg, lane, zero_tag(lane), loc);
            const std::string next = lane + 1 < kLanes ? fresh() : zero;
            const int c = zero_callee(lane, loc);
            main_.line({w.ref(kMoved), w.ref(reg_at(index_reg(loc)).name + "__M")}, c,
                       add_site(c, {next, nonzero}));
            if (lane + 1 < kLanes) {
                main_.label(next, loc);
                main_.line({w.restore(kMoved)});
            }
        }
        main_.label(after, loc);
        main_.line({w.restore(kMoved)});
    }

    // `in`: reg := C1, then the input routine returns A = the operand that turns C1 into the word.
    void input(int reg, const std::optional<std::string>& eof, SourceLoc loc) {
        const Words w{loc};
        reset(main_, reg, loc);
        const std::string ret = fresh();
        const std::string ret_eof = eof ? fresh() : ret;
        const int c = in_callee(loc);
        main_.line({w.ref(kMoved), w.ref("_INSUB")}, c, add_site(c, {ret, ret_eof}));
        if (eof) {
            main_.label(ret_eof, loc);
            main_.line({w.restore(kMoved)});
            access(main_, reg, loc);
            main_.line({w.ref(kMoved), w.ref(*eof)});
        }
        main_.label(ret, loc);
        main_.line({w.restore(kMoved)});
        access(main_, reg, loc);
        use(kIn);
    }

    // A := the low byte of reg, as a number (for Out and Hlt).
    void to_byte(int reg, SourceLoc loc) {
        const Words w{loc};
        lane_index(main_, reg, 0, kByteTag, loc);
        if (byte_ < 0) {
            byte_ = new_callee(true, loc, 1);
        }
        const std::string ret = fresh();
        main_.line({w.ref(kMoved), w.ref(reg_at(index_reg(loc)).name + "__M")}, byte_,
                   add_site(byte_, {ret}));
        main_.label(ret, loc);
        main_.line({w.restore(kMoved)});
    }

    // Primitives (docs/hla.md 4.2).

    // A := v, cells unchanged.
    void load(Stream& s, Value v, SourceLoc loc) {
        const Words w{loc};
        const Load l = hla::load(v);
        s.line({w.ref(kRot), w.lit(l.start), w.restore(kRot)});
        use(kRot);
        for (const Value& k : l.oprs) {
            s.line({w.ref(kOpr), w.lit(k), w.restore(kOpr)});
            use(kOpr);
        }
    }

    // A := reg := crz(A, reg).
    void access(Stream& s, int reg_ix, SourceLoc loc) {
        const Words w{loc};
        const std::string ret = fresh();
        const int c = reg_at(reg_ix).callee;
        s.line({w.ref(kMoved), w.ref(reg_at(reg_ix).name + "__O")}, c, add_site(c, {ret}));
        s.label(ret, loc);
        s.line({w.restore(kMoved)});
        use(kMoved);
        use(kOpr);
    }

    // reg := C1 whatever it held, and A = C1.
    void reset(Stream& s, int reg, SourceLoc loc) {
        load(s, make_value({}, 1), loc);
        access(s, reg, loc);
        access(s, reg, loc);
    }

    // The index register := the index of reg's byte lane in the table `tag` (word.hpp).
    void lane_index(Stream& s, int reg, unsigned lane, unsigned tag, SourceLoc loc) {
        const int ix = index_reg(loc);
        reset(s, ix, loc);
        access(s, reg, loc);
        access(s, ix, loc);
        load(s, lane_mask(lane, tag), loc);
        access(s, ix, loc);
        access(s, ix, loc);
    }

    // Registers and callees.

    int add_reg(const std::string& name, Value init, bool dispatch, SourceLoc loc) {
        regs_.push_back(Reg{name, init, dispatch, new_callee(false, loc, 1)});
        reg_by_name_[name] = static_cast<int>(regs_.size() - 1);
        return static_cast<int>(regs_.size() - 1);
    }
    int reg_index(const std::string& name) const { return reg_by_name_.at(name); }
    const Reg& reg_at(int i) const { return regs_[static_cast<std::size_t>(i)]; }

    int index_reg(SourceLoc loc) {
        if (index_ < 0) {
            index_ = add_reg("_I", make_value({}, 1), true, loc);
        }
        return index_;
    }
    int input_reg(SourceLoc loc) {
        if (input_ < 0) {
            input_ = add_reg("_S", make_value({}, 1), true, loc);
        }
        return input_;
    }

    int new_callee(bool sub, SourceLoc loc, std::size_t exits) {
        callees_.push_back(Callee{sub, loc, std::vector<std::vector<std::string>>(exits)});
        return static_cast<int>(callees_.size() - 1);
    }
    std::size_t add_site(int callee, std::vector<std::string> targets) {
        Callee& c = callees_[static_cast<std::size_t>(callee)];
        assert(targets.size() == c.exits.size());
        for (std::size_t e = 0; e < targets.size(); ++e) {
            c.exits[e].push_back(std::move(targets[e]));
        }
        return c.exits[0].size() - 1;
    }
    int zero_callee(unsigned lane, SourceLoc loc) {
        if (zero_[lane] < 0) {
            zero_[lane] = new_callee(true, loc, 2);
        }
        return zero_[lane];
    }
    int in_callee(SourceLoc loc) {
        if (in_ < 0) {
            in_ = new_callee(true, loc, 2);
        }
        return in_;
    }

    std::string fresh() { return "_k" + std::to_string(next_++); }
    void use(const std::string& unit) { units_.insert(unit); }

    // Output.

    // The return tree of `leaves` (docs/hla.md 4.3): a flag per depth; a set flag sends the caller
    // to the upper half (and clears itself), a clear one falls through to the lower half.
    void tree(Stream& s, const std::vector<std::string>& leaves, bool sub, SourceLoc loc,
              std::size_t lo, std::size_t hi, unsigned depth) {
        const Words w{loc};
        if (hi - lo == 1) {
            s.line({w.ref(kMoved), w.ref(leaves[lo])});
            return;
        }
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::string upper = fresh();
        const std::string f = flag_name(sub, depth);
        s.line({w.ref(f), w.ref(upper), w.restore(f)});
        depth_[sub] = std::max(depth_[sub], depth + 1);
        tree(s, leaves, sub, loc, lo, mid, depth + 1);
        s.label(upper, loc);
        tree(s, leaves, sub, loc, mid, hi, depth + 1);
    }
    void tree(Stream& s, int callee, std::size_t exit) {
        const Callee& c = callees_[static_cast<std::size_t>(callee)];
        tree(s, c.exits[exit], c.sub, c.loc, 0, c.exits[exit].size(), 0);
    }

    void block(Stream& s, std::optional<Value> at = std::nullopt) {
        as::Block b;
        b.section = as::Section::Data;
        b.braced = true;
        for (Line& l : s.lines) {
            std::vector<Cell> cells;
            if (l.callee >= 0) {
                const Callee& c = callees_[static_cast<std::size_t>(l.callee)];
                const Words w{l.cells.front().word.loc};
                for (const unsigned d : site_flags(c.exits[0].size(), l.site)) {
                    cells.push_back(w.restore(flag_name(c.sub, d)));
                }
            }
            for (Cell& c : l.cells) {
                cells.push_back(std::move(c));
            }
            cells.front().starts_line = true;
            cells.front().labels.insert(cells.front().labels.begin(), l.labels.begin(),
                                        l.labels.end());
            for (Cell& c : cells) {
                b.cells.push_back(std::move(c));
            }
        }
        assert(s.pending.empty() && !b.cells.empty());
        b.loc = b.cells.front().word.loc;
        if (at) {
            b.offset = as::Offset{Expr::literal(*at, b.loc), b.loc};
        }
        data_.push_back(std::move(b));
    }

    void code(const std::string& name, std::vector<CoreOp> ops, bool jmp = true) {
        as::Block b;
        b.section = as::Section::Code;
        Cell c;
        c.labels.push_back({name, {0, 1, 1}});
        c.word.kind = ops.size() == 1 ? as::Word::Kind::Command : as::Word::Kind::Cycle;
        c.word.loc = {0, 1, 1};
        c.word.ops = std::move(ops);
        b.cells.push_back(std::move(c));
        if (jmp) {
            Cell j;
            j.word.kind = as::Word::Kind::Command;
            j.word.loc = {0, 1, 1};
            j.word.ops = {CoreOp::Jmp};
            b.cells.push_back(std::move(j));
        }
        b.loc = {0, 1, 1};
        out_.blocks.push_back(std::move(b));
    }

    // The input routine (docs/hla.md 4.4): S := in_seed(), In, S := crz(byte, S), A := 0 and MovD
    // through S into the input table.
    void input_routine(Stream& s) {
        const SourceLoc loc = callees_[static_cast<std::size_t>(in_)].loc;
        const Words w{loc};
        const int sr = input_reg(loc);
        s.label("_INSUB", loc);
        s.line({w.restore(kMoved)});
        reset(s, sr, loc);
        load(s, *crz_operand(make_value({}, 1), in_seed()), loc);
        access(s, sr, loc);
        s.line({w.ref(kIn), w.unused(), w.restore(kIn)});
        access(s, sr, loc);
        load(s, Value{}, loc);
        s.line({w.ref(kMoved), w.ref(reg_at(sr).name + "__M")});
    }

    // The input table: a byte lands on a one-word entry `U__GO Y` with Y a word of the same block
    // naming the byte's stream; Y is the first free cell after the entry, so the RNop run before
    // _GO stays short. EOF has its own block in head 0.
    void input_tables() {
        const SourceLoc loc = callees_[static_cast<std::size_t>(in_)].loc;
        const Words w{loc};
        std::map<std::uint64_t, int> entry; // address -> byte
        for (int b = 0; b < 256; ++b) {
            entry[in_index(b).raw() + 1] = b;
        }
        const std::uint64_t first = entry.begin()->first;
        std::map<std::uint64_t, int> yword; // address -> byte
        for (const auto& [at, b] : entry) {
            std::uint64_t y = at + 1;
            while (entry.contains(y) || yword.contains(y)) {
                ++y;
            }
            yword[y] = b;
        }
        const std::uint64_t last = std::max(entry.rbegin()->first, yword.rbegin()->first);
        Stream t;
        std::vector<Cell> cells;
        for (std::uint64_t a = first; a <= last; ++a) {
            if (const auto e = entry.find(a); e != entry.end()) {
                cells.push_back(labeled(w.use(kGo, "_iy" + std::to_string(e->second)),
                                        "_it" + std::to_string(e->second)));
            } else if (const auto y = yword.find(a); y != yword.end()) {
                cells.push_back(labeled(w.ref("_ib" + std::to_string(y->second)),
                                        "_iy" + std::to_string(y->second)));
            } else {
                cells.push_back(w.unused());
            }
        }
        t.line(std::move(cells));
        block(t, Value::from_raw(first));

        // The streams: A is 0, so one Opr loads the operand that turns C1 into the word.
        Stream s;
        for (int b = 0; b < 256; ++b) {
            s.label("_ib" + std::to_string(b), loc);
            s.line({w.restore(kGo), w.restore(kMoved), w.ref(kOpr),
                    w.lit(*crz_operand(make_value({}, 1), word(static_cast<std::uint32_t>(b)))),
                    w.restore(kOpr), w.ref(kMoved), w.ref("_IN_B")});
        }
        block(s);
        Stream e;
        e.label("_IE", loc);
        e.line({w.restore(kMoved), w.ref(kOpr),
                w.lit(*crz_operand(make_value({}, 1), word(0xFFFFFFFF))), w.restore(kOpr),
                w.ref(kMoved), w.ref("_IN_E")});
        block(e, Value::from_raw(in_index(-1).raw() + 1));
        use(kGo);
    }

    // The byte table: per byte, A := the byte and back to the callers.
    void byte_table() {
        const Words w{callees_[static_cast<std::size_t>(byte_)].loc};
        for (unsigned b = 0; b < 256; ++b) {
            Stream s;
            s.label("_bt" + std::to_string(b), w.loc);
            s.line({w.restore(kMd), w.restore(kMoved)});
            load(s, *Value::natural(b), w.loc);
            s.line({w.ref(kMoved), w.ref("_BYTE_RET")});
            std::size_t cells = 0;
            for (const Line& l : s.lines) {
                cells += l.cells.size();
            }
            assert(cells <= kByteEntryCells);
            (void)cells;
            block(s, Value::from_raw(
                         hla::lane_index(0, kByteTag, static_cast<std::uint8_t>(b)).raw() + 1));
        }
    }

    void zero_tables(unsigned lane) {
        const Words w{callees_[static_cast<std::size_t>(zero_[lane])].loc};
        const std::string l = std::to_string(lane);
        for (unsigned b = 0; b < 256; ++b) {
            Stream s;
            s.label("_zt" + l + "_" + std::to_string(b), w.loc);
            s.line({w.ref(kGo), w.ref((b == 0 ? "_Z" : "_NZ") + l)});
            block(s, Value::from_raw(
                         hla::lane_index(lane, zero_tag(lane), static_cast<std::uint8_t>(b)).raw() +
                         1));
        }
        use(kGo);
    }

    void finish() {
        Stream in_routine;
        if (in_ >= 0) {
            input_routine(in_routine);
        }
        std::vector<Stream> exits;
        const auto exit = [&](const std::string& name, int callee, std::size_t e,
                              std::vector<std::string> restores) {
            const Words w{callees_[static_cast<std::size_t>(callee)].loc};
            Stream s;
            s.label(name, w.loc);
            std::vector<Cell> cells;
            for (const std::string& r : restores) {
                cells.push_back(w.restore(r));
            }
            s.line(std::move(cells));
            tree(s, callee, e);
            exits.push_back(std::move(s));
        };
        if (byte_ >= 0) {
            exit("_BYTE_RET", byte_, 0, {kMoved});
        }
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            if (zero_[lane] >= 0) {
                exit("_Z" + std::to_string(lane), zero_[lane], 0, {kMd, kGo, kMoved});
                exit("_NZ" + std::to_string(lane), zero_[lane], 1, {kMd, kGo, kMoved});
            }
        }
        if (in_ >= 0) {
            exit("_IN_B", in_, 0, {kMoved, kMd});
            exit("_IN_E", in_, 1, {kMoved, kMd});
        }
        std::vector<Stream> reg_blocks;
        for (const Reg& r : regs_) {
            const Callee& c = callees_[static_cast<std::size_t>(r.callee)];
            if (c.exits[0].empty()) {
                continue;
            }
            const Words w{c.loc};
            Stream s;
            if (r.dispatch) {
                s.label(r.name + "__M", c.loc);
                s.line({w.use(kMd, r.name)});
                use(kMd);
            }
            s.label(r.name + "__O", c.loc);
            s.line(
                {w.ref(kOpr), labeled(w.lit(r.init), r.name), w.restore(kOpr), w.restore(kMoved)});
            tree(s, r.callee, 0);
            reg_blocks.push_back(std::move(s));
        }

        block(main_);
        if (in_ >= 0) {
            block(in_routine);
        }
        for (Stream& s : exits) {
            block(s);
        }
        for (Stream& s : reg_blocks) {
            block(s);
        }
        if (byte_ >= 0) {
            byte_table();
        }
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            if (zero_[lane] >= 0) {
                zero_tables(lane);
            }
        }
        if (in_ >= 0) {
            input_tables();
        }

        const std::array<std::pair<const std::string*, std::vector<CoreOp>>, 8> units = {{
            {&kMoved, {CoreOp::MovD, CoreOp::Nop}},
            {&kRot, {CoreOp::Rot, CoreOp::Nop}},
            {&kOpr, {CoreOp::Opr, CoreOp::Nop}},
            {&kIn, {CoreOp::In, CoreOp::Nop}},
            {&kOut, {CoreOp::Out, CoreOp::Nop}},
            {&kMd, {CoreOp::MovD, CoreOp::Nop}},
            {&kGo, {CoreOp::MovD, CoreOp::Nop}},
            {&kHalt, {CoreOp::Hlt}},
        }};
        for (const auto& [name, ops] : units) {
            if (units_.contains(*name)) {
                code(*name, ops, *name != kHalt);
            }
        }
        for (const bool sub : {false, true}) {
            for (unsigned d = 0; d < depth_[sub]; ++d) {
                code(flag_name(sub, d), {CoreOp::Nop, CoreOp::MovD});
            }
        }
        for (as::Block& b : data_) {
            out_.blocks.push_back(std::move(b));
        }
    }

    const Program& p_;
    as::Program out_;
    std::vector<as::Block> data_;
    Stream main_;
    std::vector<Reg> regs_;
    std::unordered_map<std::string, int> reg_by_name_;
    std::vector<Callee> callees_;
    std::set<std::string> units_;
    int index_ = -1;
    int input_ = -1;
    int byte_ = -1;
    std::array<int, kLanes> zero_ = {-1, -1, -1, -1};
    int in_ = -1;
    unsigned next_ = 0;
    std::array<unsigned, 2> depth_ = {0, 0};
};

} // namespace

assembler::Program lower(const Program& program) { return Lowerer(program).run(); }

assembler::Program translate(const std::string& path, assembler::FileLoader& loader,
                             Diagnostics& diags) {
    const Program p = parse_file(path, loader, diags);
    if (diags.has_errors()) {
        assembler::Program out;
        out.files = p.files;
        return out;
    }
    assembler::Program out = lower(p);
    assembler::check_program(out, diags);
    return out;
}

} // namespace malm::hla
