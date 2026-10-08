#include "asm/startup.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "core/ops.hpp"
#include "core/placement.hpp"

namespace malm::assembler {

unsigned initial_width(std::uint64_t length) {
    return std::max(10u, Value::natural(length - 1)->width() + 1);
}

namespace {

using core::Op;
using Raw = std::uint64_t;

// ---- The low area (startup.hpp) ----

constexpr Raw kCodeStart = 99; // the startup code; cell 0's Jmp continues here with D = 1
constexpr Raw kGraphEnd = 98;  // cells 1..97 are static; 98 is encrypted by that Jmp

// The farthest a target may lie above the base in ACC_P: X - B Nops per trip against a new
// pointer (measured: 8 is about best for bytes and for wide values at n = 10..20).
constexpr std::uint64_t kSlide = 8;

// Registers.
constexpr Raw kZ1 = 44;
constexpr Raw kQ = 45;
constexpr Raw kC0 = 47;
constexpr Raw kZ0 = 48;
constexpr Raw kSeed = 49;
constexpr Raw kVal = 51; // ACC_V
constexpr Raw kPtr = 53; // ACC_P
constexpr Raw kF1 = 55;
constexpr Raw kF2 = 57;

// The fill (startup.hpp): y at the end of the text; the cell after y is the home pointer.
constexpr std::uint8_t kFillY = 85; // 0t10011
constexpr Raw kHome = kFillY + 1;

struct Pointer {
    Raw cell;
    Raw land; // a MovD at `cell` continues at `land`: the cell holds land - 1
};

// The SEED loop: 50 -> SEED; after ACC_V, 52 -> Z0 (one Nop from SEED, so a Z0 operation is
// cheap); after ACC_P, 54 -> 46, from where the Nops pass C0 and Z0. The others were chosen by a
// greedy search over the loadable landings of each cell, weighted by how often a route is taken.
constexpr std::array<Pointer, 17> kPointers{{{1, 81},
                                             {8, 55},
                                             {36, 63},
                                             {38, 44},
                                             {42, 57},
                                             {46, 53},
                                             {50, 49},
                                             {52, 48},
                                             {54, 46},
                                             {56, 44},
                                             {58, 42},
                                             {61, 57},
                                             {63, 55},
                                             {81, 54},
                                             {kHome, 48},
                                             {87, 47},
                                             {88, 46}}};

struct Initial {
    Raw cell;
    std::uint8_t ch;
};

// Initial characters: 0/1-trit characters for the registers that start as C1 or C0, 54 = 0t2000
// for Q.
constexpr std::array<Initial, 7> kInitial{
    {{kZ1, 37}, {kQ, 54}, {kC0, 109}, {kZ0, 85}, {kSeed, 84}, {kF1, 120}, {kF2, 118}}};

constexpr bool loadable(Raw pos, unsigned ch) {
    return ch >= 33 && ch <= 126 &&
           core::normalize(pos, static_cast<std::uint8_t>(ch)) != core::kNoSymbol;
}

constexpr std::array<std::uint8_t, kCodeStart> kLow = [] {
    std::array<std::uint8_t, kCodeStart> t{};
    for (Raw p = 0; p < kCodeStart; ++p) {
        t[p] = core::loadable_char(p, Op::Nop);
    }
    t[0] = core::loadable_char(0, Op::Jmp);
    for (const Pointer& q : kPointers) {
        t[q.cell] = static_cast<std::uint8_t>(q.land - 1);
    }
    for (const Initial& i : kInitial) {
        t[i.cell] = i.ch;
    }
    return t;
}();

constexpr bool low_area_ok() {
    for (Raw p = 0; p < kCodeStart; ++p) {
        if (!loadable(p, kLow[p])) {
            return false;
        }
    }
    for (const Pointer& q : kPointers) {
        if (q.cell == 0 || q.cell >= kGraphEnd || q.land == 0 || q.land >= kGraphEnd) {
            return false;
        }
    }
    return kLow[0] == 'b';
}
static_assert(low_area_ok(), "every low-area cell must be loadable, pointers inside 1..97");

// Moves through the static graph of cells 1..97: Nop (p -> p + 1) or MovD along a pointer.
class Routes {
  public:
    Routes() {
        std::array<Raw, kGraphEnd> land{};
        for (const Pointer& q : kPointers) {
            land[q.cell] = q.land;
        }
        for (Raw s = 1; s < kGraphEnd; ++s) {
            auto& first = first_[s];
            auto& dist = dist_[s];
            dist.fill(kUnreachable);
            dist[s] = 0;
            std::deque<Raw> queue{s};
            while (!queue.empty()) {
                const Raw p = queue.front();
                queue.pop_front();
                const std::pair<Raw, Op> moves[] = {{p + 1, Op::Nop}, {land[p], Op::MovD}};
                for (const auto& [q, op] : moves) {
                    if (q == 0 || q >= kGraphEnd || dist[q] != kUnreachable) {
                        continue;
                    }
                    dist[q] = dist[p] + 1;
                    first[q] = p == s ? op : first[p];
                    queue.push_back(q);
                }
            }
        }
    }
    static const Routes& get() {
        static const Routes r;
        return r;
    }
    unsigned dist(Raw from, Raw to) const { return dist_[from][to]; }
    Op first(Raw from, Raw to) const { return first_[from][to]; }

    static constexpr unsigned kUnreachable = 0xFFFF;

  private:
    std::array<std::array<unsigned, kGraphEnd>, kGraphEnd> dist_{};
    std::array<std::array<Op, kGraphEnd>, kGraphEnd> first_{};
};

// ---- Trit tables ----

// crz per trit (spec 3.2): kCrz[d][a].
constexpr std::uint8_t kCrz[3][3] = {{1, 0, 0}, {1, 0, 2}, {2, 2, 1}};
// What one operation does to a register trit in the frame (startup.hpp): Opr with A = C0, and the
// hot trit of a SEED operation after an even / odd number of S-type operations.
constexpr std::uint8_t kAfterC0[3] = {1, 1, 2};
constexpr std::uint8_t kKappa[2][3] = {{0, 0, 1}, {0, 2, 0}};

// A set of trit values as bits; S swaps the values 1 and 2.
constexpr std::uint8_t swap12(std::uint8_t set) {
    return static_cast<std::uint8_t>((set & 1) | ((set & 2) << 1) | ((set & 4) >> 1));
}
constexpr bool has(std::uint8_t set, unsigned t) { return ((set >> t) & 1) != 0; }

// The values a register may end with: per low position, and for the head (bits for 0 and 1).
struct Goal {
    std::array<std::uint8_t, core::kMaxWidth> low{};
    std::uint8_t head = 0;
    bool need_op = false; // at least one operation, so that A ends equal to the register
};

Goal exact(Value v, unsigned n) {
    Goal g;
    for (unsigned j = 0; j < n; ++j) {
        g.low[j] = static_cast<std::uint8_t>(1u << v.trit(j));
    }
    g.head = static_cast<std::uint8_t>(1u << v.head());
    return g;
}

Value nat(std::uint64_t k) { return *Value::natural(k); }

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& why) { throw Failure(why); }

class Generator {
  public:
    Generator(const Layout& l, Raw base, unsigned n0)
        : l_(l), base_(base), width_(l.width), n_(n0), d_(nat(1)) {}

    Startup run() {
        if (n_ > width_) {
            fail("the program text needs n = " + std::to_string(n_) + " from the start");
        }
        bootstrap();
        force_width();
        build_machinery();
        for (const ImageCell& c : l_.cells) {
            write_target(c.addr, c.value);
        }
        const auto entry = l_.labels.find("ENTRY");
        if (entry == l_.labels.end()) {
            fail("no ENTRY");
        }
        build(kPtr, exact(*subtract(entry->second.addr, nat(1)), width_));
        at(kPtr, Op::MovD);
        code_.push_back(core::symbol_of(Op::Jmp));
        return finish();
    }

  private:
    const Layout& l_;
    Raw base_;
    unsigned width_; // the program width N
    unsigned n_;     // the current n
    Value a_;
    Value d_;
    std::unordered_map<Raw, Value> mem_; // every cell written so far
    std::vector<std::uint8_t> code_;     // instruction symbols, from cell 99 on
    unsigned hot_ = 0;                   // the hot position of SEED
    std::optional<Value> base_ptr_;      // the base B whose B - 1 ACC_P holds

    // ---- The simulated machine ----

    static bool in_graph(Value v) { return v.head() == 0 && v.raw() >= 1 && v.raw() < kGraphEnd; }

    // Cells that are not the low area, written or fill are unknown: cell 0, cell 98, the startup
    // code and everything up to `base`, which D must never read.
    std::optional<Value> read(Value a) const {
        if (const auto it = mem_.find(a.raw()); it != mem_.end()) {
            return it->second;
        }
        if (a.head() == 0 && a.raw() < base_) {
            if (in_graph(a)) {
                return nat(kLow[a.raw()]);
            }
            return std::nullopt;
        }
        const Value y = nat(kFillY);
        return a.mod6() % 2 == 0 ? y : core::crz(y, y); // crz(y, y) swaps y's 0 and 1 trits
    }

    Value known(Value a) const {
        const auto v = read(a);
        if (!v) {
            fail("D reached the unknown cell " + a.to_string());
        }
        return *v;
    }

    void emit(Op op) {
        switch (op) {
        case Op::MovD:
            d_ = known(d_);
            break;
        case Op::Opr:
            a_ = core::crz(a_, known(d_));
            mem_[d_.raw()] = a_;
            break;
        case Op::Rot: {
            const Value v = known(d_);
            if (!v.fits_width(n_)) {
                fail("rotating a value wider than n");
            }
            a_ = core::rotr(v, n_);
            mem_[d_.raw()] = a_;
            break;
        }
        case Op::Nop:
            break;
        default:
            fail("unexpected instruction");
        }
        const auto next = d_.incremented();
        if (!next) {
            fail("D overflows");
        }
        d_ = *next;
        if (!d_.fits_width(n_ - 1)) {
            n_ = d_.width() + 1;
            if (n_ > width_) {
                fail("D reached " + d_.to_string() + ", wider than the program width allows");
            }
        }
        code_.push_back(core::symbol_of(op));
    }

    void route(Raw to) {
        const Routes& r = Routes::get();
        while (d_.raw() != to || !in_graph(d_)) {
            if (!in_graph(d_) || r.dist(d_.raw(), to) == Routes::kUnreachable) {
                fail("no route from " + d_.to_string() + " to cell " + std::to_string(to));
            }
            emit(r.first(d_.raw(), to));
        }
    }

    void at(Raw reg, Op op) {
        route(reg);
        emit(op);
    }

    // After writing a cell D is outside the low area: Nop forward to a cell whose MovD chain leads
    // back without growing n, then follow it.
    void escape() {
        for (int tries = 0; tries < 64; ++tries) {
            if (in_graph(d_)) {
                return;
            }
            if (leads_home(d_)) {
                while (!in_graph(d_)) {
                    emit(Op::MovD);
                }
                return;
            }
            const auto next = d_.incremented();
            if (!next || !next->fits_width(n_ - 1)) {
                break;
            }
            emit(Op::Nop);
        }
        fail("no way back to the work area from " + d_.to_string());
    }

    bool leads_home(Value p) const {
        for (int hop = 0; hop < 4; ++hop) {
            const auto v = read(p);
            const auto land = v ? v->incremented() : std::nullopt;
            if (!land || !land->fits_width(n_ - 1)) {
                return false;
            }
            if (in_graph(*land)) {
                return true;
            }
            p = *land;
        }
        return false;
    }

    void expect(Raw reg, Value want, const char* what) const {
        if (known(nat(reg)) != want) {
            fail(std::string("bootstrap: ") + what + " is " + known(nat(reg)).to_string());
        }
    }

    // ---- Bootstrap ----

    void bootstrap() {
        const Value c1 = core::crz(Value(), Value()); // ...111
        at(kF1, Op::Opr);                             // A = 0: C1
        at(kC0, Op::Opr);                             // A = C1: 0
        for (const Raw r : {kF2, kZ1, kZ0, kSeed}) {
            at(kC0, Op::Rot);
            at(r, Op::Opr);
            expect(r, c1, "a C1 register");
        }
        expect(kF1, c1, "F1");
        expect(kC0, Value(), "C0");
    }

    // Rot at the first fill cell from D whose trit 0 differs from its head (rotr then has width n),
    // then back to the work area.
    void rotate_fill() {
        for (int tries = 0; tries < 16; ++tries) {
            const auto v = read(d_);
            if (v && v->trit(0) != v->head() && v->fits_width(n_)) {
                emit(Op::Rot);
                escape();
                return;
            }
            emit(Op::Nop);
        }
        fail("width forcing: no fill cell to rotate");
    }

    void force_width() {
        if (n_ == width_) {
            return;
        }
        at(kF1, Op::MovD); // through C1: D = ...1112, a fill cell
        rotate_fill();
        Raw cur = kF2;
        Raw other = kF1;
        while (n_ < width_) {
            const unsigned m = n_;
            at(cur, Op::Opr); // cur = swap01(A): width m, no trit 2
            const auto land = known(nat(cur)).incremented();
            if (!land || land->fits_width(m - 1) || !land->fits_width(m)) {
                fail("width forcing: a pointer of the wrong width");
            }
            at(kC0, Op::Rot);
            at(other, Op::Opr);
            expect(other, core::crz(Value(), Value()), "a forcing register");
            at(cur, Op::MovD);
            if (n_ != m + 1) {
                fail("width forcing grew n by more than one");
            }
            rotate_fill();
            std::swap(cur, other);
        }
    }

    void build_machinery() {
        for (unsigned k = 0; k < width_; ++k) {
            at(kQ, Op::Rot); // A = the single 2 of Q, one position lower
            at(kZ1, Op::Opr);
        }
        const Value low2 = nat(core::pow3[width_] - 1);
        const Value z1 = Value::from_raw((core::pow3[core::kTrits] - 1) / 2 + // head 1,
                                         (core::pow3[width_] - 1) / 2);       // low n trits 2
        expect(kZ1, z1, "Z1");
        at(kZ1, Op::Rot);
        at(kZ0, Op::Opr);
        expect(kZ0, low2, "Z0");
        at(kZ1, Op::Rot);
        at(kSeed, Op::Opr); // low n trits 2, head 0
        at(kQ, Op::Rot);
        at(kSeed, Op::Opr); // the trit under Q's 2 becomes 1, head 1
        const Value seed = known(nat(kSeed));
        unsigned ones = 0;
        for (unsigned j = 0; j < width_; ++j) {
            if (seed.trit(j) == 1) {
                hot_ = j;
                ++ones;
            }
        }
        if (ones != 1 || seed.head() != 1) {
            fail("bootstrap: SEED is " + seed.to_string());
        }
    }

    // ---- Building a value in ACC_V or ACC_P ----

    struct Visit {
        bool dummy = false; // a Z0 operation first, to fix the parity
        bool insert = false;
    };

    struct Plan {
        bool c0 = false;
        std::vector<Visit> visits; // one per rotation of SEED
        std::vector<Raw> tail;     // final Z operations
        unsigned cost = 0;
    };

    unsigned cost_of(Raw reg, Raw from) const {
        const Routes& r = Routes::get();
        return r.dist(kSeed + 1, from) + 1 + r.dist(from + 1, reg) + 1 + r.dist(reg + 1, kSeed);
    }

    // The cheapest plan, by dynamic programming over the visited positions with the state (parity
    // of the S-type operations so far, whether any operation ran). At a position the hot trit can
    // be left alone, inserted, or inserted after a Z0 operation that flips the parity; the plan
    // may stop where every remaining position already holds an allowed value.
    std::optional<Plan> plan(Raw reg, const Goal& g) const {
        constexpr unsigned kInf = ~0u / 2;
        const Value cur = known(nat(reg));
        const Routes& r = Routes::get();
        const unsigned rot_skip = 1 + r.dist(kSeed + 1, kSeed);
        const unsigned rot_insert = 1 + r.dist(kSeed + 1, reg) + 1 + r.dist(reg + 1, kSeed);
        const unsigned dummy = cost_of(reg, kZ0);
        struct Cell {
            unsigned cost = kInf;
            std::uint8_t c = 0; // the predecessor's state
            std::uint8_t any = 0;
            Visit visit;
        };
        std::optional<Plan> best;
        std::vector<std::uint8_t> xs(width_);
        std::vector<std::uint8_t> wants(width_);
        std::vector<bool> settled(width_ + 1);
        std::vector<std::array<std::array<Cell, 2>, 2>> dp(width_ + 1);
        for (const bool c0 : {false, true}) {
            for (unsigned pi = 0; pi < 2; ++pi) {
                for (unsigned k = 0; k < width_; ++k) {
                    const unsigned j = (hot_ + 2 * width_ - 1 - k) % width_;
                    xs[k] = static_cast<std::uint8_t>(c0 ? kAfterC0[cur.trit(j)] : cur.trit(j));
                    wants[k] = pi ? swap12(g.low[j]) : g.low[j];
                }
                settled[width_] = true;
                for (unsigned k = width_; k-- > 0;) {
                    settled[k] = settled[k + 1] && has(wants[k], xs[k]);
                }
                for (auto& row : dp) {
                    row = {};
                }
                dp[0][0][0].cost = c0 ? cost_of(reg, kC0) : 0;
                const auto relax = [&](unsigned k, unsigned c, unsigned any, unsigned cost,
                                       unsigned from_c, unsigned from_any, Visit v) {
                    Cell& t = dp[k][c][any];
                    if (cost < t.cost) {
                        t = {cost, static_cast<std::uint8_t>(from_c),
                             static_cast<std::uint8_t>(from_any), v};
                    }
                };
                for (unsigned k = 0; k < width_; ++k) {
                    for (unsigned c = 0; c < 2; ++c) {
                        for (unsigned any = 0; any < 2; ++any) {
                            const unsigned cost = dp[k][c][any].cost;
                            if (cost >= kInf) {
                                continue;
                            }
                            const unsigned x = xs[k];
                            if (has(wants[k], x)) {
                                relax(k + 1, c, any, cost + rot_skip, c, any, {});
                            }
                            if (has(wants[k], kKappa[c][x])) {
                                relax(k + 1, c ^ 1, 1, cost + rot_insert, c, any, {false, true});
                            }
                            if (has(wants[k], kKappa[c ^ 1][x])) {
                                relax(k + 1, c, 1, cost + dummy + rot_insert, c, any, {true, true});
                            }
                        }
                    }
                }
                const std::vector<std::vector<Raw>> tails = {
                    {}, {kZ0}, {kZ1}, {kZ0, kZ0}, {kZ0, kZ1}};
                for (unsigned k = 0; k <= width_; ++k) {
                    if (!settled[k]) {
                        continue;
                    }
                    for (unsigned c = 0; c < 2; ++c) {
                        for (unsigned any = 0; any < 2; ++any) {
                            if (dp[k][c][any].cost >= kInf) {
                                continue;
                            }
                            const unsigned head = any ? 0 : (c0 ? 1 : cur.head());
                            for (const auto& tail : tails) {
                                const unsigned h =
                                    tail.empty() ? head : (tail.back() == kZ0 ? 1 : 0);
                                if ((c + tail.size()) % 2 != pi || !has(g.head, h) ||
                                    (g.need_op && !c0 && !any && tail.empty())) {
                                    continue;
                                }
                                unsigned cost = dp[k][c][any].cost;
                                for (const Raw z : tail) {
                                    cost += cost_of(reg, z);
                                }
                                if (best && cost >= best->cost) {
                                    continue;
                                }
                                Plan p;
                                p.c0 = c0;
                                p.tail = tail;
                                p.cost = cost;
                                p.visits.resize(k);
                                unsigned bc = c;
                                unsigned bany = any;
                                for (unsigned i = k; i > 0; --i) {
                                    const Cell& t = dp[i][bc][bany];
                                    p.visits[i - 1] = t.visit;
                                    bc = t.c;
                                    bany = t.any;
                                }
                                best = std::move(p);
                            }
                        }
                    }
                }
            }
        }
        return best;
    }

    void build(Raw reg, Goal g) {
        const auto p = plan(reg, g);
        if (!p) {
            fail("no plan for a register value");
        }
        if (p->c0) {
            at(kC0, Op::Rot);
            at(reg, Op::Opr);
        }
        for (const Visit& v : p->visits) {
            if (v.dummy) {
                at(kZ0, Op::Rot);
                at(reg, Op::Opr);
            }
            at(kSeed, Op::Rot);
            hot_ = (hot_ + width_ - 1) % width_;
            if (v.insert) {
                at(reg, Op::Opr);
            }
        }
        for (const Raw z : p->tail) {
            at(z, Op::Rot);
            at(reg, Op::Opr);
        }
        const Value v = known(nat(reg));
        bool ok = has(g.head, v.head());
        for (unsigned j = 0; j < width_; ++j) {
            ok = ok && has(g.low[j], v.trit(j));
        }
        if (!ok || (g.need_op && a_ != v)) {
            fail("a register value came out wrong: " + v.to_string());
        }
    }

    // ---- Writing the image ----

    // The values a with crz(a, d) = v, per trit; false if some trit has none.
    bool solve(Value d, Value v, Goal& g) const {
        for (unsigned j = 0; j < width_; ++j) {
            g.low[j] = 0;
            for (unsigned a = 0; a < 3; ++a) {
                if (kCrz[d.trit(j)][a] == v.trit(j)) {
                    g.low[j] = static_cast<std::uint8_t>(g.low[j] | (1u << a));
                }
            }
            if (g.low[j] == 0) {
                return false;
            }
        }
        g.head = 0;
        for (unsigned a = 0; a < 2; ++a) {
            if (kCrz[d.head()][a] == v.head()) {
                g.head = static_cast<std::uint8_t>(g.head | (1u << a));
            }
        }
        return g.head != 0;
    }

    void point_to(Value x) {
        if (base_ptr_ && base_ptr_->head() == x.head() && base_ptr_->raw() <= x.raw() &&
            x.raw() - base_ptr_->raw() <= kSlide) {
            return;
        }
        build(kPtr, exact(*subtract(x, nat(1)), width_));
        base_ptr_ = x;
    }

    // With A prepared: D to x through ACC_P and the Nop slide, Opr, back.
    void trip(Value x) {
        at(kPtr, Op::MovD);
        while (d_ != x) {
            emit(Op::Nop);
        }
        emit(Op::Opr);
        escape();
    }

    void write_target(Value x, Value v) {
        if (x.head() == 2 || v.head() == 2) {
            fail("a head-2 image cell");
        }
        point_to(x);
        Goal g;
        if (!solve(known(x), v, g)) {
            at(kC0, Op::Rot);
            trip(x); // [x] := crz(0, [x]): every 0 trit becomes 1
            if (!solve(known(x), v, g)) {
                fail("cannot write " + v.to_string() + " at " + x.to_string());
            }
        }
        g.need_op = true;
        build(kVal, g);
        trip(x);
        if (known(x) != v) {
            fail("wrote the wrong value at " + x.to_string());
        }
    }

    // ---- The text ----

    Startup finish() const {
        Startup s;
        s.code_cells = code_.size();
        s.steps = code_.size();
        s.text.assign(kLow.begin(), kLow.end());
        for (std::size_t i = 0; i < code_.size(); ++i) {
            s.text.push_back(core::denormalize(kCodeStart + i, code_[i]));
        }
        // The two last cells: some 0/1-trit character x, then y, at an odd length.
        static constexpr std::uint8_t kZeroOne[] = {36, 37, 39,  40,  81,  82,  84,  85,  90,  91,
                                                    93, 94, 108, 109, 111, 112, 117, 118, 120, 121};
        for (Raw len = s.text.size() + 2;; ++len) {
            if (len % 2 == 0 || !loadable(len - 1, kFillY)) {
                continue;
            }
            for (const std::uint8_t x : kZeroOne) {
                if (loadable(len - 2, x)) {
                    while (s.text.size() < len - 2) {
                        s.text.push_back(core::loadable_char(s.text.size(), Op::Nop));
                    }
                    s.text.push_back(x);
                    s.text.push_back(kFillY);
                    return s;
                }
            }
        }
    }
};

} // namespace

std::optional<Startup> generate_startup(const Layout& layout, std::uint64_t base,
                                        unsigned initial_width, std::string& error) {
    try {
        return Generator(layout, base, initial_width).run();
    } catch (const Failure& f) {
        error = f.what();
        return std::nullopt;
    }
}

} // namespace malm::assembler
