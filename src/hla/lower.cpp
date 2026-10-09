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
const std::string kMd = "_MD";    // MovD through a dispatch register
const std::string kGo = "_GO";    // MovD through the next word: a goto out of a table
const std::string kRa = "_RA";    // RAM access: MovD through a pointer register, then Opr the word
const std::string kMb = "_MB";    // MovD out of a record to _BACK
const std::string kDir = "_DIR";  // flag: the stack pointer moves up
const std::string kCarry = "_FC"; // flag: the ALU's carry
const std::string kNeg = "_FN";   // flag: sra of a negative word (flip around the shift)
const std::string kLeft = "_FSL"; // flag: a computed shift goes left
const std::string kK9 = "_K9";    // counter: every 9th pass ends a rotation loop

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
    Cell unused() const { return special(as::Word::Kind::Unused); }
    Cell unknown() const { return special(as::Word::Kind::Unknown); }
    Cell special(as::Word::Kind kind) const {
        Cell c;
        c.word.kind = kind;
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
    int callee = -1;       // path 0: the accesses of the program
    bool ram = false;      // also a `U_RA` entry: a pointer register
    // Paths 1, 2, ...: callees of their own, chosen by the flags _P1, _P2, ... that the caller
    // sets, so a routine's accesses do not deepen the tree of the program's (docs/hla.md 4.6).
    std::vector<int> paths;
    int rot = 0; // the path that is a Rot access, looped until _K9 fires; 0 if none
};

std::string path_flag(int path) { return "_P" + std::to_string(path); }

// A switch statement's table: the target of every byte.
struct SwitchTable {
    SourceLoc loc;
    std::vector<std::string> targets; // 256
};

class Lowerer {
  public:
    explicit Lowerer(const Program& p) : p_(p) {}

    as::Program run() {
        out_.files = p_.files;
        out_.width = as::Width{kWidth, SourceLoc{0, 1, 1}};
        for (const RamDecl& r : p_.rams) {
            ram_base_[r.name] = r.base;
        }
        for (const RegDecl& r : p_.regs) {
            add_reg(reg_name(r.name), value_of(r.init), false, r.loc);
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
        switch (s.op) {
        case Op::Mov: {
            const int dst = reg_of(s.args[0]);
            if (!is_reg(s.args[1])) {
                mov_value(main_, dst, value_of(s.args[1]), s.loc);
            } else {
                mov_reg(main_, dst, reg_of(s.args[1]), s.loc);
            }
            return true;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Slt:
        case Op::Sltu:
            alu_op(s);
            return true;
        case Op::And:
        case Op::Or:
        case Op::Xor:
            bitwise(s);
            return true;
        case Op::Sll:
        case Op::Srl:
        case Op::Sra:
            shift(s);
            return true;
        case Op::Sextb:
        case Op::Sexth:
            sext(reg_of(s.args[0]), reg_of(s.args[1]), s.op == Op::Sextb ? 8 : 16, s.loc);
            return true;
        case Op::Jmp:
            go(s.args[0], s.loc);
            return false;
        case Op::Call: {
            const std::string ret = fresh();
            push(Operand{}, code_address(code_id(ret, s.loc)), s.loc);
            go(s.args[0], s.loc);
            main_.label(ret, s.loc);
            main_.line({w.restore(kMoved)});
            return true;
        }
        case Op::Ret: {
            stack_move(main_, false, s.loc);
            const int r = return_reg(s.loc);
            load_word(main_, r, stack_reg(s.loc), s.loc);
            main_.line({w.ref(kMoved), w.ref(reg_at(r).name + "__M")});
            use(kMd);
            return false;
        }
        case Op::Push:
            push(s.args[0], is_reg(s.args[0]) ? Value{} : value_of(s.args[0]), s.loc);
            return true;
        case Op::Pop:
            stack_move(main_, false, s.loc);
            load_word(main_, reg_of(s.args[0]), stack_reg(s.loc), s.loc);
            return true;
        case Op::Switch:
            switch_table(s);
            return true;
        case Op::Load:
            load_word(main_, reg_of(s.args[0]), pointer(s.args[1], s.loc), s.loc);
            return true;
        case Op::Store:
            store_word(main_, pointer(s.args[0], s.loc), s.args[1],
                       is_reg(s.args[1]) ? Value{} : value_of(s.args[1]), s.loc);
            return true;
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

    // Values and names (docs/hla.md 3).

    bool is_reg(const Operand& o) const {
        return o.kind == Operand::Kind::Name && reg_by_name_.contains(reg_name(o.name));
    }
    int reg_of(const Operand& o) const { return reg_index(reg_name(o.name)); }

    // A number, a RAM region (its base address) or a label (its code address).
    Value value_of(const Operand& o) {
        if (o.kind == Operand::Kind::Imm) {
            return word(imm_word(o.imm));
        }
        if (const auto r = ram_base_.find(o.name); r != ram_base_.end()) {
            return word(r->second);
        }
        return code_address(code_id(label_name(o.name), o.loc));
    }

    // The id of the code address of `label` (a landing pad of the main stream, docs/hla.md 4.5).
    std::uint32_t code_id(const std::string& label, SourceLoc loc) {
        const auto [it, fresh] =
            code_ids_.emplace(label, static_cast<std::uint32_t>(code_pads_.size() + 1));
        if (fresh) {
            code_pads_.emplace_back(label, loc);
        }
        return it->second;
    }

    // Continue at a label, or at the code address a register holds.
    void go(const Operand& target, SourceLoc loc) {
        const Words w{loc};
        if (is_reg(target)) {
            const int r = reg_of(target);
            reg_mut(r).dispatch = true;
            main_.line({w.ref(kMoved), w.ref(reg_at(r).name + "__M")});
            use(kMd);
        } else {
            main_.line({w.ref(kMoved), w.ref(label_name(target.name))});
        }
        use(kMoved);
    }

    // `switch r, ...` (docs/hla.md 4.4): the lane-0 index of r, moved into the switch's own table,
    // whose entries go to the labels or to the next statement.
    void switch_table(const Stmt& s) {
        const Words w{s.loc};
        const auto n = static_cast<std::uint32_t>(switches_.size());
        const int ix = index_reg(s.loc);
        lane_index(main_, reg_of(s.args[0]), 0, kSwitchTag, s.loc);
        load(main_, switch_mask(n), s.loc);
        access(main_, ix, s.loc);
        main_.line({w.ref(kMoved), w.ref(reg_at(ix).name + "__M")});
        const std::string after = fresh();
        SwitchTable t{s.loc, std::vector<std::string>(256, after)};
        for (std::size_t b = 0; b + 1 < s.args.size(); ++b) {
            t.targets[b] = label_name(s.args[b + 1].name);
        }
        switches_.push_back(std::move(t));
        main_.label(after, s.loc);
        main_.line({w.restore(kMoved)});
        use(kMd);
    }

    // RAM and the stack (docs/hla.md 4.5).

    // A := [word] := crz(A, [word]) for the word the pointer register p points at.
    void ram_access(Stream& s, int p, SourceLoc loc) {
        const Words w{loc};
        if (ram_ < 0) {
            ram_ = new_callee(false, loc, 1);
        }
        reg_mut(p).ram = true;
        const std::string ret = fresh();
        s.line({w.ref(kMoved), w.ref(reg_at(p).name + "__A")}, ram_, add_site(ram_, {ret}));
        s.label(ret, loc);
        s.line({w.restore(kMoved)});
        use(kMoved);
    }

    // The pointer register := the pointer to the word at an address: a register (crz only,
    // word.hpp) or a constant.
    int pointer(const Operand& addr, SourceLoc loc) {
        if (ptr_ < 0) {
            ptr_ = add_reg("_P", make_value({}, 1), false, loc);
        }
        reset(main_, ptr_, loc);
        if (is_reg(addr)) {
            const int x = reg_of(addr);
            load(main_, pointer_mask(), loc);
            access(main_, x, loc);
            access(main_, ptr_, loc);
            load(main_, pointer_restore(), loc);
            access(main_, x, loc);
        } else {
            const std::uint32_t a =
                addr.kind == Operand::Kind::Imm ? imm_word(addr.imm) : ram_base_.at(addr.name);
            load(main_, *crz_operand(make_value({}, 1), ram_pointer(a)), loc);
            access(main_, ptr_, loc);
        }
        return ptr_;
    }

    // reg := the word p points at (read once, with A = C1).
    void load_word(Stream& s, int reg, int p, SourceLoc loc) {
        reset(s, reg, loc);
        ram_access(s, p, loc);
        access(s, reg, loc);
        load(s, make_value({}, 1), loc);
        access(s, reg, loc);
    }

    // The word p points at := a register, or the value v if `src` is not one. The word is reset
    // to C1 and written once; a register is read with A = 0 and restored (ADR 0001: three
    // accesses to the word, two to the register).
    void store_word(Stream& s, int p, const Operand& src, Value v, SourceLoc loc) {
        load(s, make_value({}, 1), loc);
        ram_access(s, p, loc);
        ram_access(s, p, loc);
        if (is_reg(src)) {
            const int x = reg_of(src);
            load(s, Value{}, loc);
            access(s, x, loc);
            ram_access(s, p, loc);
            load(s, make_value({}, 1), loc);
            access(s, x, loc);
        } else {
            load(s, *crz_operand(make_value({}, 1), v), loc);
            ram_access(s, p, loc);
        }
    }

    // Push a register, or the value v if `src` is not one.
    void push(const Operand& src, Value v, SourceLoc loc) {
        store_word(main_, stack_reg(loc), src, v, loc);
        stack_move(main_, true, loc);
    }

    // The stack pointer moves one frame up or down: a MovD through it lands on the frame's entry,
    // whose stream loads the operand that turns the pointer into its neighbour (or halts past
    // either end).
    void stack_move(Stream& s, bool up, SourceLoc loc) {
        const Words w{loc};
        const int sp = stack_reg(loc);
        const std::string ret = fresh();
        std::vector<Cell> cells;
        if (up) {
            cells.push_back(w.restore(kDir));
        }
        cells.push_back(w.ref(kMoved));
        cells.push_back(w.ref(reg_at(sp).name + "__M"));
        s.line(std::move(cells), move_, add_site(move_, {ret}));
        s.label(ret, loc);
        s.line({w.restore(kMoved)});
        use(kMoved);
        use(kMd);
        use(kGo);
        use(kDir);
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

    // Arithmetic (docs/hla.md 4.6, docs/adr/0002-hla-arithmetic.md).

    // The word of a constant operand: a number or a RAM region's base (labels are rejected).
    std::uint32_t const_of(const Operand& o) const {
        return o.kind == Operand::Kind::Imm ? imm_word(o.imm) : ram_base_.at(o.name);
    }

    // add, sub, sltu, slt: column sums into _AS, then the ALU routine runs the nibbles with the
    // carry (in: _FC) and leaves the sum in _AZ and the carry-out in its exit.
    void alu_op(const Stmt& st) {
        const Words w{st.loc};
        const int d = reg_of(st.args[0]);
        const int x = reg_of(st.args[1]);
        const bool yreg = is_reg(st.args[2]);
        const int y = yreg ? reg_of(st.args[2]) : -1;
        if (yreg && y == x && st.op != Op::Add) {
            mov_value(main_, d, Value{}, st.loc); // x - x, x < x
            return;
        }
        // The carry-in: 1 for x + ~y + 1 (sub of a register, the compares).
        const bool cin = st.op != Op::Add && (yreg || st.op != Op::Sub);
        const int sr = sum_reg(st.loc);
        if (yreg) {
            Digits g;
            g.fill(st.op == Op::Add ? Digit::Add : Digit::Sub);
            if (st.op != Op::Slt) {
                run(main_, sum_sequence(g), {{x, 0}, {y, 0}, {sr, 0}}, st.loc);
            } else {
                // x ^ 0x80000000 and y ^ 0x80000000 compared unsigned: with x's bit 31 flipped
                // the top digit is (1 - x31) + y31, an Add digit.
                g[kWordBits - 1] = Digit::Add;
                const Sequence flip = transform(bit_map(
                    [](unsigned i) { return i == kWordBits - 1 ? Trit::Flip : Trit::Keep; }));
                run(main_, flip, {{x, 0}}, st.loc);
                run(main_, sum_sequence(g), {{x, 0}, {y, 0}, {sr, 0}}, st.loc);
                run(main_, flip, {{x, 0}}, st.loc);
            }
        } else {
            // x + k; x - k = x + -k; sltu: x + ~k + 1; slt: the same with the top digit of
            // (x ^ 0x80000000) + ~(k ^ 0x80000000), that is (1 - x31) + k31.
            const std::uint32_t k = const_of(st.args[2]);
            const std::uint32_t e = st.op == Op::Add ? k : st.op == Op::Sub ? 0 - k : ~k;
            Digits g;
            for (unsigned i = 0; i < kWordBits; ++i) {
                g[i] = (e >> i) & 1 ? Digit::XPlus1 : Digit::X;
            }
            if (st.op == Op::Slt) {
                g[kWordBits - 1] = (k >> 31) & 1 ? Digit::NotXPlus1 : Digit::NotX;
            }
            run(main_, sum_sequence_imm(g), {{x, 0}, {sr, 0}}, st.loc);
        }
        if (st.op == Op::Add || st.op == Op::Sub) {
            const std::string ret = fresh();
            alu_call(cin, ret, ret, st.loc);
            main_.label(ret, st.loc);
            main_.line({w.restore(kMoved)});
            mov_reg(main_, d, z_reg(st.loc), st.loc);
            return;
        }
        // A compare: the carry-out is 1 iff x >= y.
        reset(main_, d, st.loc);
        const std::string lt = fresh();
        const std::string ge = fresh();
        const std::string end = fresh();
        alu_call(true, lt, ge, st.loc);
        main_.label(lt, st.loc);
        main_.line({w.restore(kMoved)});
        load(main_, *crz_operand(make_value({}, 1), word(1)), st.loc);
        access(main_, d, st.loc);
        main_.line({w.ref(kMoved), w.ref(end)});
        main_.label(ge, st.loc);
        main_.line({w.restore(kMoved)});
        load(main_, *crz_operand(make_value({}, 1), word(0)), st.loc);
        access(main_, d, st.loc);
        main_.line({w.ref(kMoved), w.ref(end)});
        main_.label(end, st.loc);
        main_.line({w.restore(kMoved)});
    }

    // Calls the ALU routine with the carry-in; it returns to `no_carry` or `carry`.
    void alu_call(bool cin, const std::string& no_carry, const std::string& carry, SourceLoc loc) {
        const Words w{loc};
        if (alu_ < 0) {
            alu_ = new_callee(true, loc, 2);
        }
        std::vector<Cell> cells;
        if (cin) {
            cells.push_back(w.restore(kCarry));
        }
        cells.push_back(w.ref(kMoved));
        cells.push_back(w.ref("_ALU"));
        main_.line(std::move(cells), alu_, add_site(alu_, {no_carry, carry}));
        use(kMoved);
        use(kCarry);
    }

    // and, or, xor: with a register, a sequence into the destination (or a scratch register if the
    // destination is an operand); with a constant, a transform of the destination.
    void bitwise(const Stmt& st) {
        const int d = reg_of(st.args[0]);
        const int x = reg_of(st.args[1]);
        if (!is_reg(st.args[2])) {
            const std::uint32_t k = const_of(st.args[2]);
            const TritMap m = bit_map([&](unsigned i) {
                const bool b = (k >> i) & 1;
                switch (st.op) {
                case Op::And:
                    return b ? Trit::Keep : Trit::Clear;
                case Op::Or:
                    return b ? Trit::Fill : Trit::Keep;
                default:
                    return b ? Trit::Flip : Trit::Keep;
                }
            });
            mov_reg(main_, d, x, st.loc);
            bool any = false;
            for (const Trit t : m) {
                any = any || t != Trit::Keep;
            }
            if (any) {
                run(main_, transform(m), {{d, 0}}, st.loc);
            }
            return;
        }
        const int y = reg_of(st.args[2]);
        if (x == y) {
            if (st.op == Op::Xor) {
                mov_value(main_, d, Value{}, st.loc);
            } else {
                mov_reg(main_, d, x, st.loc);
            }
            return;
        }
        const int z = d == x || d == y ? scratch_reg(st.loc) : d;
        const Sequence seq = st.op == Op::And  ? and_sequence()
                             : st.op == Op::Or ? or_sequence()
                                               : xor_sequence();
        run(main_, seq, {{x, 0}, {y, 0}, {z, 0}}, st.loc);
        mov_reg(main_, d, z, st.loc);
    }

    // sll, srl, sra: the shift register _H := x, the shift routine rotates it (at the entry of a
    // constant amount, or through a switch table on the amount's low byte), d := _H. sra of a
    // negative word shifts its complement and flips back (the routine sees _FN).
    void shift(const Stmt& st) {
        const Words w{st.loc};
        const int d = reg_of(st.args[0]);
        const int x = reg_of(st.args[1]);
        const bool yreg = is_reg(st.args[2]);
        const bool left = st.op == Op::Sll;
        if (!yreg && st.args[2].imm == 0) {
            mov_reg(main_, d, x, st.loc);
            return;
        }
        const int h = shift_reg(st.loc);
        mov_reg(main_, h, x, st.loc);
        if (st.op == Op::Sra) {
            const std::string neg = fresh();
            const std::string join = fresh();
            sign_test(h, kLanes - 1, join, neg, st.loc);
            main_.label(neg, st.loc);
            main_.line({w.restore(kMoved)});
            run(main_, flip_all(), {{h, 0}}, st.loc);
            main_.line({w.restore(kNeg), w.ref(kMoved), w.ref(join)});
            main_.label(join, st.loc);
            main_.line({w.restore(kMoved)});
            use(kNeg);
        }
        if (shift_ < 0) {
            shift_ = new_callee(true, st.loc, 1);
        }
        const std::string ret = fresh();
        if (!yreg) {
            const auto k = static_cast<unsigned>(st.args[2].imm);
            shift_entries_.insert({left, k});
            main_.line({w.ref(kMoved), w.ref(shift_entry(left, k))}, shift_,
                       add_site(shift_, {ret}));
        } else {
            // The amount's low byte through a switch table of its own: entry b goes to _SV<b & 31>,
            // which tests _FSL.
            if (shift_switch_ < 0) {
                shift_switch_ = static_cast<int>(switches_.size());
                SwitchTable t{st.loc, std::vector<std::string>(256)};
                for (unsigned b = 0; b < 256; ++b) {
                    t.targets[b] = "_SV" + std::to_string(b % kWordBits);
                }
                switches_.push_back(std::move(t));
                for (unsigned k = 0; k < kWordBits; ++k) {
                    shift_entries_.insert({false, k});
                    if (k > 0) {
                        shift_entries_.insert({true, k});
                    }
                }
            }
            const int ix = index_reg(st.loc);
            lane_index(main_, reg_of(st.args[2]), 0, kSwitchTag, st.loc);
            load(main_, switch_mask(static_cast<std::uint32_t>(shift_switch_)), st.loc);
            access(main_, ix, st.loc);
            std::vector<Cell> cells;
            if (left) {
                cells.push_back(w.restore(kLeft));
                use(kLeft);
            }
            cells.push_back(w.ref(kMoved));
            cells.push_back(w.ref(reg_at(ix).name + "__M"));
            main_.line(std::move(cells), shift_, add_site(shift_, {ret}));
            use(kMd);
        }
        main_.label(ret, st.loc);
        main_.line({w.restore(kMoved)});
        mov_reg(main_, d, h, st.loc);
    }

    static std::string shift_entry(bool left, unsigned k) {
        return (left ? "_SL" : "_SR") + std::to_string(k);
    }

    static Sequence flip_all() {
        return transform(bit_map([](unsigned) { return Trit::Flip; }));
    }

    // sextb, sexth: d := x, then the bits from `bits` up cleared or filled by the sign.
    void sext(int d, int x, unsigned bits, SourceLoc loc) {
        const Words w{loc};
        mov_reg(main_, d, x, loc);
        const std::string pos = fresh();
        const std::string neg = fresh();
        const std::string end = fresh();
        sign_test(d, bits / 8 - 1, pos, neg, loc);
        for (const bool negative : {true, false}) {
            main_.label(negative ? neg : pos, loc);
            main_.line({w.restore(kMoved)});
            run(main_, transform(bit_map([&](unsigned i) {
                    return i < bits ? Trit::Keep : negative ? Trit::Fill : Trit::Clear;
                })),
                {{d, 0}}, loc);
            main_.line({w.ref(kMoved), w.ref(end)});
        }
        main_.label(end, loc);
        main_.line({w.restore(kMoved)});
    }

    // Continues at the landing pad `pos` or `neg` by the top bit of reg's byte lane, through the
    // lane's sign table. Nothing follows the dispatch line.
    void sign_test(int reg, unsigned lane, const std::string& pos, const std::string& neg,
                   SourceLoc loc) {
        const Words w{loc};
        const int ix = index_reg(loc);
        index_sequence(main_, reg, sign_mask(lane), loc);
        if (sign_[lane] < 0) {
            sign_[lane] = new_callee(true, loc, 2);
        }
        main_.line({w.ref(kMoved), w.ref(reg_at(ix).name + "__M")}, sign_[lane],
                   add_site(sign_[lane], {pos, neg}));
        use(kMd);
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

    // A := reg := crz(A, reg), through one of the register's paths; through its Rot path:
    // reg := rotr(reg) until _K9 fires (9 times from a fresh counter).
    void access(Stream& s, int reg_ix, SourceLoc loc, int path = 0) {
        const Words w{loc};
        const std::string ret = fresh();
        const Reg& r = reg_at(reg_ix);
        const int c = path == 0 ? r.callee : r.paths[static_cast<std::size_t>(path - 1)];
        std::vector<Cell> cells;
        if (path > 0) {
            cells.push_back(w.restore(path_flag(path)));
            max_path_ = std::max(max_path_, path);
        }
        cells.push_back(w.ref(kMoved));
        cells.push_back(w.ref(r.name + (path > 0 && path == r.rot ? "__R" : "__O")));
        s.line(std::move(cells), c, add_site(c, {ret}));
        s.label(ret, loc);
        s.line({w.restore(kMoved)});
        use(kMoved);
        use(path > 0 && path == r.rot ? kRot : kOpr);
    }

    // reg := C1 whatever it held, and A = C1.
    void reset(Stream& s, int reg, SourceLoc loc, int path = 0) {
        load(s, make_value({}, 1), loc);
        access(s, reg, loc, path);
        access(s, reg, loc, path);
    }

    // dst := the word v.
    void mov_value(Stream& s, int dst, Value v, SourceLoc loc) {
        reset(s, dst, loc);
        load(s, *crz_operand(make_value({}, 1), v), loc);
        access(s, dst, loc);
    }

    // dst := src. dst := C1; A := src; dst := crz(src, C1) (0 -> 1, 2 -> 2); with A = C1 the 1s
    // become 0. src may also hold its word as 1/2 instead of 0/2 (the ALU's result): the first
    // access with A = C1 turns it into 0/2.
    void mov_reg(Stream& s, int dst, int src, SourceLoc loc) {
        if (src == dst) {
            return;
        }
        reset(s, dst, loc);
        access(s, src, loc);
        access(s, dst, loc);
        load(s, make_value({}, 1), loc);
        access(s, dst, loc);
    }

    // A sequence of word.hpp; cells[i] is the register (and its path) of the sequence's cell i.
    void run(Stream& s, const Sequence& seq, const std::vector<std::pair<int, int>>& cells,
             SourceLoc loc) {
        for (const Step& st : seq) {
            if (st.cell < 0) {
                load(s, st.k, loc);
            } else {
                const auto& [reg, path] = cells[static_cast<std::size_t>(st.cell)];
                access(s, reg, loc, path);
            }
        }
    }

    // The index register := the index of reg's byte lane in the table `tag` (word.hpp).
    void lane_index(Stream& s, int reg, unsigned lane, unsigned tag, SourceLoc loc) {
        index_sequence(s, reg, lane_mask(lane, tag), loc);
    }
    // The lane index sequence with the constant `mask` (lane_mask, sign_mask).
    void index_sequence(Stream& s, int reg, Value mask, SourceLoc loc) {
        const int ix = index_reg(loc);
        reset(s, ix, loc);
        access(s, reg, loc);
        access(s, ix, loc);
        load(s, mask, loc);
        access(s, ix, loc);
        access(s, ix, loc);
    }

    // Registers and callees.

    int add_reg(const std::string& name, Value init, bool dispatch, SourceLoc loc) {
        assert(!reg_by_name_.contains(name));
        Reg r;
        r.name = name;
        r.init = init;
        r.dispatch = dispatch;
        r.callee = new_callee(false, loc, 1);
        regs_.push_back(std::move(r));
        reg_by_name_[name] = static_cast<int>(regs_.size() - 1);
        return static_cast<int>(regs_.size() - 1);
    }
    int reg_index(const std::string& name) const { return reg_by_name_.at(name); }
    const Reg& reg_at(int i) const { return regs_[static_cast<std::size_t>(i)]; }
    Reg& reg_mut(int i) { return regs_[static_cast<std::size_t>(i)]; }

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
    // The stack pointer: a pointer register at frame 0, the bottom of the stack.
    int stack_reg(SourceLoc loc) {
        if (sp_ < 0) {
            sp_ = add_reg("_SP", Value::from_raw(frame_record(0).raw() - 2), true, loc);
            reg_mut(sp_).ram = true;
            move_ = new_callee(true, loc, 1);
        }
        return sp_;
    }
    // The register `ret` jumps through.
    int return_reg(SourceLoc loc) {
        if (ret_ < 0) {
            ret_ = add_reg("_R", make_value({}, 1), true, loc);
        }
        return ret_;
    }

    // The ALU's registers: the column sums _AS and the result _AZ (both C1 between uses; the
    // ALU's own accesses on path 1), and the nibble index registers _AJ0 .. _AJ7.
    int sum_reg(SourceLoc loc) {
        if (sum_ < 0) {
            sum_ = add_reg("_AS", make_value({}, 1), false, loc);
            add_path(sum_, loc);
            z_ = add_reg("_AZ", make_value({}, 1), false, loc);
            add_path(z_, loc);
            for (unsigned k = 0; k < kNibbles; ++k) {
                nib_[k] = add_reg("_AJ" + std::to_string(k), make_value({}, 1), true, loc);
            }
        }
        return sum_;
    }
    int z_reg(SourceLoc loc) {
        sum_reg(loc);
        return z_;
    }
    // The destination of and/or/xor when it is also an operand.
    int scratch_reg(SourceLoc loc) {
        if (scratch_ < 0) {
            scratch_ = add_reg("_T", Value{}, false, loc);
        }
        return scratch_;
    }
    // The shift register: path 1 rotates it, path 2 is the shift routine's.
    int shift_reg(SourceLoc loc) {
        if (h_ < 0) {
            h_ = add_reg("_H", Value{}, false, loc);
            reg_mut(h_).rot = add_path(h_, loc);
            add_path(h_, loc);
        }
        return h_;
    }
    int add_path(int reg, SourceLoc loc) {
        reg_mut(reg).paths.push_back(new_callee(false, loc, 1));
        return static_cast<int>(reg_at(reg).paths.size());
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

    // A landing pad at `at` (a code address + 1 or a switch entry): reached by MovD through a
    // dispatch register, it re-arms _MD and _MOVED and continues at `target`.
    void pad(const std::string& name, const std::string& target, SourceLoc loc, Value at) {
        const Words w{loc};
        Stream s;
        s.label(name, loc);
        s.line({w.restore(kMd), w.restore(kMoved), w.ref(kMoved), w.ref(target)});
        block(s, at);
        use(kMd);
        use(kMoved);
    }

    // _RA: MovD through the pointer register (record - 2), an RNop, Opr on the record's word, and
    // its Jmp continues at the record's MB. 4-cycles, because a MovD/Nop and an Opr/Nop 2-cycle
    // are too far apart mod 94 (docs/adr/0001).
    void ram_unit() {
        as::Block b;
        b.section = as::Section::Code;
        b.loc = {0, 1, 1};
        const auto cell = [&](as::Word::Kind kind, std::vector<CoreOp> ops) {
            Cell c;
            c.word.kind = kind;
            c.word.loc = {0, 1, 1};
            c.word.ops = std::move(ops);
            b.cells.push_back(std::move(c));
        };
        cell(as::Word::Kind::Cycle, {CoreOp::MovD, CoreOp::Nop, CoreOp::Nop, CoreOp::Nop});
        b.cells.back().labels.push_back({kRa, {0, 1, 1}});
        cell(as::Word::Kind::RNop, {});
        cell(as::Word::Kind::Cycle, {CoreOp::Opr, CoreOp::Nop, CoreOp::Nop, CoreOp::Nop});
        cell(as::Word::Kind::Command, {CoreOp::Jmp});
        out_.blocks.push_back(std::move(b));
    }

    // Where every RAM access ends: _MB moved D here. Re-arm _MB and _MOVED, step both 4-cycles of
    // _RA back to their first state (each R__RA runs the RNop and the Opr cell as a Nop, so the
    // next word is three cells on), and return.
    Stream ram_back() {
        const Words w{callees_[static_cast<std::size_t>(ram_)].loc};
        Stream s;
        s.label("_BACK", w.loc);
        std::vector<Cell> cells = {w.restore(kMb), w.restore(kMoved)};
        for (int i = 0; i < 3; ++i) {
            cells.push_back(w.restore(kRa));
            cells.push_back(w.unused());
            cells.push_back(w.unused());
        }
        s.line(std::move(cells));
        tree(s, ram_, 0);
        return s;
    }

    // The records of the declared RAM: `? [value] MB BACK` from record - 1; the `?` keeps the
    // value out of the first word of a line, where 0 is not allowed (docs/asm.md 6).
    void ram_records() {
        for (const RamDecl& r : p_.rams) {
            const Words w{r.loc};
            for (std::uint32_t i = 0; i < r.size; ++i) {
                const std::uint32_t a = r.base + 4 * i;
                Stream s;
                s.label("_r" + std::to_string(a), r.loc);
                s.line({w.unknown(), w.lit(i < r.init.size() ? value_of(r.init[i]) : Value{}),
                        w.ref(kMb), w.ref("_BACK")});
                block(s, Value::from_raw(ram_record(a).raw() - 1));
            }
        }
    }

    // The stack's shared exits: _SP_RET applies the operand a frame's stream loaded to the stack
    // pointer and returns; _SOVF halts with 255 (overflow or underflow).
    void stack_exits(std::vector<Stream>& exits) {
        const Words w{callees_[static_cast<std::size_t>(move_)].loc};
        Stream s;
        s.label("_SP_RET", w.loc);
        s.line({w.restore(kMoved)});
        access(s, sp_, w.loc);
        tree(s, move_, 0);
        exits.push_back(std::move(s));
        Stream o;
        o.label("_SOVF", w.loc);
        o.line({w.restore(kMoved)});
        load(o, *Value::natural(255), w.loc);
        o.line({w.ref(kHalt)});
        use(kHalt);
        exits.push_back(std::move(o));
    }

    // Frames 0..N (N = the stack size; the pointer is at frame k with k words pushed). A frame is
    // a record `[entry][value][MB][BACK][Y]` from record - 1: a MovD through the stack pointer
    // lands on the entry `U__GO Y`, and Y names the frame's stream, which tests _DIR and loads
    // the operand that turns the pointer into the next or the previous frame's.
    void frames() {
        const Words w{callees_[static_cast<std::size_t>(move_)].loc};
        const std::uint32_t n = p_.stack ? p_.stack->size : kDefaultStackFrames;
        const auto ptr = [](std::uint32_t f) { return Value::from_raw(frame_record(f).raw() - 2); };
        for (std::uint32_t f = 0; f <= n; ++f) {
            const std::string k = std::to_string(f);
            Stream r;
            r.label("_f" + k, w.loc);
            r.line({w.use(kGo, "_fy" + k), w.lit(Value{}), w.ref(kMb), w.ref("_BACK"),
                    labeled(w.ref("_fs" + k), "_fy" + k)});
            block(r, Value::from_raw(frame_record(f).raw() - 1));

            Stream s;
            s.label("_fs" + k, w.loc);
            s.line({w.restore(kGo), w.restore(kMd), w.restore(kMoved)});
            s.line({w.ref(kDir), w.ref("_fu" + k), w.restore(kDir)});
            const auto to = [&](std::optional<std::uint32_t> g) {
                if (g) {
                    load(s, *crz_operand(ptr(f), ptr(*g)), w.loc);
                }
                s.line({w.ref(kMoved), w.ref(g ? "_SP_RET" : "_SOVF")});
            };
            to(f > 0 ? std::optional(f - 1) : std::nullopt);
            s.label("_fu" + k, w.loc);
            to(f < n ? std::optional(f + 1) : std::nullopt);
            block(s);
        }
    }

    // The ALU routine (docs/hla.md 4.6): _AZ := C1; per nibble k the mask from _AS into _AJ<k>,
    // whose last constant the carry chooses, and a dispatch into the nibble table; the entry's
    // stream loads the merge constant of the result nibble, sets _FC on a carry-out and continues
    // at _AC<k>, which merges it into _AZ and resets _AJ<k>. Then _AS := C1 and the exit by the
    // carry.
    void alu_routine(Stream& s) {
        const SourceLoc loc = callees_[static_cast<std::size_t>(alu_)].loc;
        const Words w{loc};
        s.label("_ALU", loc);
        s.line({w.restore(kMoved)});
        reset(s, z_, loc, 1);
        for (unsigned k = 0; k < kNibbles; ++k) {
            const int j = nib_[k];
            run(s, mask_prefix(k), {{sum_, 1}, {j, 0}}, loc);
            const std::string c1 = fresh();
            s.line({w.ref(kCarry), w.ref(c1), w.restore(kCarry)});
            for (const unsigned cin : {0U, 1U}) {
                if (cin == 1) {
                    s.label(c1, loc);
                }
                run(s, mask_last(k, cin), {{sum_, 1}, {j, 0}}, loc);
                s.line({w.ref(kMoved), w.ref(reg_at(j).name + "__M")});
            }
            s.label("_AC" + std::to_string(k), loc);
            s.line({w.restore(kMoved)});
            access(s, z_, loc, 1);
            reset(s, j, loc);
        }
        reset(s, sum_, loc, 1);
        const std::string carry = fresh();
        s.line({w.ref(kCarry), w.ref(carry), w.restore(kCarry)});
        tree(s, alu_, 0);
        s.label(carry, loc);
        tree(s, alu_, 1);
        use(kMd);
        use(kGo);
    }

    // The nibble tables: an entry `_GO stream` per (nibble, carry-in, digits), a stream per
    // (nibble, digits' total + carry-in).
    void alu_tables() {
        const Words w{callees_[static_cast<std::size_t>(alu_)].loc};
        const auto stream = [](unsigned k, unsigned total) {
            return "_as" + std::to_string(k) + "_" + std::to_string(total);
        };
        for (unsigned k = 0; k < kNibbles; ++k) {
            for (unsigned total = 0; total < 2 << kNibbleBits; ++total) {
                Stream s;
                s.label(stream(k, total), w.loc);
                s.line({w.restore(kMd), w.restore(kGo), w.restore(kMoved)});
                load(s, word((total & 15) << (kNibbleBits * k)), w.loc);
                std::vector<Cell> cells;
                if (total >> kNibbleBits) {
                    cells.push_back(w.restore(kCarry));
                }
                cells.push_back(w.ref(kMoved));
                cells.push_back(w.ref("_AC" + std::to_string(k)));
                s.line(std::move(cells));
                block(s);
            }
            for (unsigned cin = 0; cin < 2; ++cin) {
                for (unsigned code = 0; code < 81; ++code) {
                    const std::array<unsigned, kNibbleBits> d = {code % 3, code / 3 % 3,
                                                                 code / 9 % 3, code / 27};
                    const unsigned total = d[0] + 2 * d[1] + 4 * d[2] + 8 * d[3] + cin;
                    Stream e;
                    e.label("_an" + std::to_string(k) + "_" + std::to_string(cin) + "_" +
                                std::to_string(code),
                            w.loc);
                    e.line({w.ref(kGo), w.ref(stream(k, total))});
                    block(e, Value::from_raw(nibble_index(k, cin, d).raw() + 1));
                }
            }
        }
    }

    // _H rotated right m times: loops of 9 rotations (the counter _K9 fires on every 9th pass and
    // is back at its start then), the first one shortened by advancing the counter first.
    void rotate(Stream& s, unsigned m, SourceLoc loc) {
        const Words w{loc};
        const int rot = reg_at(h_).rot;
        if (m % 9 != 0) {
            for (unsigned i = 0; i < 9 - m % 9; ++i) {
                const std::string next = fresh();
                s.line({w.ref(kK9), w.ref(next)});
                s.label(next, loc);
            }
            access(s, h_, loc, rot);
        }
        for (unsigned i = 0; i < m / 9; ++i) {
            access(s, h_, loc, rot);
        }
        use(kK9);
    }

    // The shift routine: for a computed amount _SV<k> (entered from the shift switch) tests _FSL;
    // the entry of (direction, k) rotates _H and loads the first constant of the shift map's
    // transform; _SH_TAIL finishes the transform, flips back after an sra of a negative word, and
    // returns.
    void shift_routine(std::vector<Stream>& out) {
        const SourceLoc loc = callees_[static_cast<std::size_t>(shift_)].loc;
        const Words w{loc};
        if (shift_switch_ >= 0) {
            for (unsigned k = 0; k < kWordBits; ++k) {
                Stream s;
                s.label("_SV" + std::to_string(k), loc);
                s.line({w.restore(kMoved)});
                const std::string l = fresh();
                s.line({w.ref(kLeft), w.ref(l), w.restore(kLeft)});
                s.line({w.ref(kMoved), w.ref(shift_entry(false, k))});
                s.label(l, loc);
                s.line({w.ref(kMoved), w.ref(shift_entry(k > 0, k))});
                out.push_back(std::move(s));
            }
            use(kLeft);
        }
        const Sequence tail = transform(shift_map(false, 0));
        for (const auto& [left, k] : shift_entries_) {
            Stream s;
            s.label(shift_entry(left, k), loc);
            s.line({w.restore(kMoved)});
            rotate(s, rotations(left, k), loc);
            const Sequence m = transform(shift_map(left, k));
            assert(m.size() == 4 && m[0].cell < 0 && m[2].k == tail[2].k); // the short form
            load(s, m[0].k, loc);
            s.line({w.ref(kMoved), w.ref("_SH_TAIL")});
            out.push_back(std::move(s));
        }
        Stream t;
        t.label("_SH_TAIL", loc);
        t.line({w.restore(kMoved)});
        run(t, Sequence(tail.begin() + 1, tail.end()), {{h_, 2}}, loc);
        const std::string neg = fresh();
        t.line({w.ref(kNeg), w.ref(neg), w.restore(kNeg)});
        tree(t, shift_, 0);
        t.label(neg, loc);
        run(t, flip_all(), {{h_, 2}}, loc);
        tree(t, shift_, 0);
        out.push_back(std::move(t));
        use(kNeg);
    }

    void sign_tables(unsigned lane) {
        const Words w{callees_[static_cast<std::size_t>(sign_[lane])].loc};
        const std::string l = std::to_string(lane);
        for (unsigned b = 0; b < 256; ++b) {
            Stream s;
            s.label("_sg" + l + "_" + std::to_string(b), w.loc);
            s.line({w.ref(kGo), w.ref((b & 0x80 ? "_SGN" : "_SGP") + l)});
            block(s, Value::from_raw(sign_index(lane, static_cast<std::uint8_t>(b)).raw() + 1));
        }
        use(kGo);
    }

    void finish() {
        Stream in_routine;
        if (in_ >= 0) {
            input_routine(in_routine);
        }
        std::vector<Stream> routines;
        if (alu_ >= 0) {
            routines.emplace_back();
            alu_routine(routines.back());
        }
        if (shift_ >= 0) {
            shift_routine(routines);
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
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            if (sign_[lane] >= 0) {
                exit("_SGP" + std::to_string(lane), sign_[lane], 0, {kMd, kGo, kMoved});
                exit("_SGN" + std::to_string(lane), sign_[lane], 1, {kMd, kGo, kMoved});
            }
        }
        if (ram_ >= 0) {
            exits.push_back(ram_back());
        }
        if (sp_ >= 0) {
            stack_exits(exits);
        }
        std::vector<Stream> reg_blocks;
        for (const Reg& r : regs_) {
            const Callee& c = callees_[static_cast<std::size_t>(r.callee)];
            const auto path_used = [&](int p) {
                return !callees_[static_cast<std::size_t>(r.paths[static_cast<std::size_t>(p - 1)])]
                            .exits[0]
                            .empty();
            };
            bool used = !c.exits[0].empty() || r.dispatch || r.ram;
            for (int p = 1; p <= static_cast<int>(r.paths.size()); ++p) {
                used = used || path_used(p);
            }
            if (!used) {
                continue;
            }
            const Words w{c.loc};
            Stream s;
            if (r.ram) {
                s.label(r.name + "__A", c.loc);
                s.line({w.use(kRa, r.name)});
                use(kRa);
                use(kMb);
            }
            if (r.dispatch) {
                s.label(r.name + "__M", c.loc);
                s.line({w.use(kMd, r.name)});
                use(kMd);
            }
            if (r.rot > 0 && path_used(r.rot)) {
                s.label(r.name + "__R", c.loc);
                s.line({w.use(kRot, r.name)});
                use(kRot);
            }
            // After the value, the path flags choose the continuation (docs/hla.md 4.6).
            s.label(r.name + "__O", c.loc);
            std::vector<Cell> cells = {w.ref(kOpr), labeled(w.lit(r.init), r.name)};
            std::vector<std::pair<int, std::string>> paths;
            for (int p = 1; p <= static_cast<int>(r.paths.size()); ++p) {
                if (path_used(p)) {
                    paths.emplace_back(p, fresh());
                    cells.push_back(w.ref(path_flag(p)));
                    cells.push_back(w.ref(paths.back().second));
                    cells.push_back(w.restore(path_flag(p)));
                }
            }
            cells.push_back(w.restore(kOpr));
            cells.push_back(w.restore(kMoved));
            s.line(std::move(cells));
            use(kOpr);
            use(kMoved);
            if (!c.exits[0].empty()) {
                tree(s, r.callee, 0);
            }
            for (const auto& [p, l] : paths) {
                const int callee = r.paths[static_cast<std::size_t>(p - 1)];
                s.label(l, c.loc);
                if (p == r.rot) {
                    // Rotate again until _K9 fires.
                    const std::string done = fresh();
                    s.line({w.restore(kRot), w.restore(kMoved), w.ref(kK9), w.ref(done),
                            w.restore(path_flag(p)), w.ref(kMoved), w.ref(r.name + "__R")});
                    s.label(done, c.loc);
                } else {
                    s.line({w.restore(kOpr), w.restore(kMoved)});
                }
                tree(s, callee, 0);
            }
            reg_blocks.push_back(std::move(s));
        }

        block(main_);
        if (in_ >= 0) {
            block(in_routine);
        }
        for (Stream& r : routines) {
            block(r);
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
        if (alu_ >= 0) {
            alu_tables();
        }
        for (unsigned lane = 0; lane < kLanes; ++lane) {
            if (sign_[lane] >= 0) {
                sign_tables(lane);
            }
        }
        if (ram_ >= 0) {
            ram_records();
        }
        if (sp_ >= 0) {
            frames();
        }
        for (std::size_t i = 0; i < code_pads_.size(); ++i) {
            const auto& [label, loc] = code_pads_[i];
            pad("_c" + std::to_string(i + 1), label, loc,
                Value::from_raw(code_address(static_cast<std::uint32_t>(i + 1)).raw() + 1));
        }
        for (std::size_t n = 0; n < switches_.size(); ++n) {
            for (unsigned b = 0; b < 256; ++b) {
                pad("_w" + std::to_string(n) + "_" + std::to_string(b), switches_[n].targets[b],
                    switches_[n].loc,
                    Value::from_raw(
                        switch_index(static_cast<std::uint32_t>(n), static_cast<std::uint8_t>(b))
                            .raw() +
                        1));
            }
        }

        const std::array<std::pair<const std::string*, std::vector<CoreOp>>, 9> units = {{
            {&kMoved, {CoreOp::MovD, CoreOp::Nop}},
            {&kRot, {CoreOp::Rot, CoreOp::Nop}},
            {&kOpr, {CoreOp::Opr, CoreOp::Nop}},
            {&kIn, {CoreOp::In, CoreOp::Nop}},
            {&kOut, {CoreOp::Out, CoreOp::Nop}},
            {&kMd, {CoreOp::MovD, CoreOp::Nop}},
            {&kGo, {CoreOp::MovD, CoreOp::Nop}},
            {&kMb, {CoreOp::MovD, CoreOp::Nop}},
            {&kHalt, {CoreOp::Hlt}},
        }};
        for (const auto& [name, ops] : units) {
            if (units_.contains(*name)) {
                code(*name, ops, *name != kHalt);
            }
        }
        if (units_.contains(kRa)) {
            ram_unit();
        }
        for (const std::string* flag : {&kDir, &kCarry, &kNeg, &kLeft}) {
            if (units_.contains(*flag)) {
                code(*flag, {CoreOp::Nop, CoreOp::MovD});
            }
        }
        if (units_.contains(kK9)) {
            std::vector<CoreOp> ops(8, CoreOp::Nop);
            ops.push_back(CoreOp::MovD);
            code(kK9, std::move(ops));
        }
        for (int p = 1; p <= max_path_; ++p) {
            code(path_flag(p), {CoreOp::Nop, CoreOp::MovD});
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
    int ram_ = -1;  // the RAM access callee
    int ptr_ = -1;  // the pointer register of load and store
    int sp_ = -1;   // the stack pointer
    int move_ = -1; // the stack pointer's move callee
    int ret_ = -1;  // the register ret jumps through
    int sum_ = -1;  // the ALU's column sums (also _AZ, _AJ0 .. _AJ7)
    int z_ = -1;
    std::array<int, kNibbles> nib_{};
    int alu_ = -1; // the ALU routine
    int scratch_ = -1;
    int h_ = -1;     // the shift register
    int shift_ = -1; // the shift routine
    int shift_switch_ = -1;
    std::set<std::pair<bool, unsigned>> shift_entries_; // (left, amount) entries in use
    std::array<int, kLanes> sign_ = {-1, -1, -1, -1};
    int max_path_ = 0;
    std::unordered_map<std::string, std::uint32_t> ram_base_;
    std::unordered_map<std::string, std::uint32_t> code_ids_;
    std::vector<std::pair<std::string, SourceLoc>> code_pads_; // [id - 1]: the label, first use
    std::vector<SwitchTable> switches_;
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
