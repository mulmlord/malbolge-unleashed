#include "hla/word.hpp"

#include <array>
#include <cassert>
#include <stdexcept>

#include "core/ops.hpp"

namespace malm::hla {
namespace {

using core::crz;
using core::kMaxWidth;
using core::pow3;

// With A = a, crz(a, d) == d exactly for the trits d in kFixed[a] (spec 3.2).
constexpr std::array<std::array<int, 2>, 3> kFixed = {{{1, 2}, {0, 2}, {0, -1}}};

// The trits of the constants that lead from `from` to `to` in `steps` fixed-point Oprs; trit 2 is
// avoided when `no2` (the head of a data word cannot be 2, docs/asm.md 4.3).
bool path(unsigned from, unsigned to, unsigned steps, bool no2, std::vector<unsigned>& out) {
    if (steps == 0) {
        return from == to;
    }
    for (const int d : kFixed[from]) {
        if (d < 0 || (no2 && d == 2)) {
            continue;
        }
        out.push_back(static_cast<unsigned>(d));
        if (path(static_cast<unsigned>(d), to, steps - 1, no2, out)) {
            return true;
        }
        out.pop_back();
    }
    return false;
}

// The trit that is 1 in every lane index (the ADR keeps 0 there for RAM records).
constexpr unsigned kTableTrit = 36;

} // namespace

Value make_value(const std::vector<unsigned>& low, unsigned head) {
    assert(low.size() <= kMaxWidth && head < 3);
    std::uint64_t raw = head * pow3[kMaxWidth];
    for (std::size_t i = 0; i < kMaxWidth; ++i) {
        const unsigned t = i < low.size() ? low[i] : head;
        assert(t < 3);
        raw += t * pow3[i];
    }
    return Value::from_raw(raw);
}

Value word(std::uint32_t w) {
    std::uint64_t raw = 0;
    for (unsigned i = 0; i < kWordBits; ++i) {
        if ((w >> i) & 1) {
            raw += 2 * pow3[kBit0 + i];
        }
    }
    return Value::from_raw(raw);
}

std::optional<std::uint32_t> word_value(Value v) {
    std::uint32_t w = 0;
    for (unsigned i = 0; i < core::kTrits; ++i) {
        const unsigned t = v.trit(i);
        const bool bit = i >= kBit0 && i < kBit0 + kWordBits;
        if (t == 2 && bit) {
            w |= std::uint32_t{1} << (i - kBit0);
        } else if (t != 0) {
            return std::nullopt;
        }
    }
    return w;
}

std::optional<Value> crz_operand(Value from, Value to) {
    std::vector<unsigned> low(kMaxWidth);
    unsigned head = 0;
    for (unsigned i = 0; i <= kMaxWidth; ++i) {
        const unsigned f = from.trit(i);
        const unsigned t = to.trit(i);
        bool found = false;
        for (unsigned a = 0; a < 3 && !found; ++a) {
            const unsigned r = crz(make_value({}, a), make_value({}, f)).head();
            if (r == t) {
                (i == kMaxWidth ? head : low[i]) = a;
                found = true;
            }
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return make_value(low, head);
}

Load load(Value target) {
    assert(target.head() != 2 && target.fits_width(kWidth));
    for (unsigned steps = 0; steps <= 4; ++steps) {
        for (const unsigned start : {0U, 1U}) {
            // Per trit 0..kWidth-1 a path; the trits from kWidth up equal the head and share its
            // path, so every constant has width <= kWidth and a head other than 2.
            std::vector<std::vector<unsigned>> paths(kWidth + 1);
            bool ok = true;
            for (unsigned i = 0; i <= kWidth && ok; ++i) {
                const bool head = i == kWidth;
                ok = path(start, target.trit(head ? kMaxWidth : i), steps, head, paths[i]);
            }
            if (!ok) {
                continue;
            }
            Load l{make_value({}, start), {}};
            for (unsigned k = 0; k < steps; ++k) {
                std::vector<unsigned> low(kWidth);
                for (unsigned i = 0; i < kWidth; ++i) {
                    low[i] = paths[i][k];
                }
                l.oprs.push_back(make_value(low, paths[kWidth][k]));
            }
            return l;
        }
    }
    throw std::logic_error("hla::load: no load sequence"); // unreachable for valid targets
}

namespace {

// A lane index with the trit 36 = `table`, and the constant of its index sequence.
Value table_index(unsigned lane, unsigned tag, std::uint8_t byte, unsigned table) {
    std::vector<unsigned> low(kWidth, 1);
    for (unsigned i = 0; i < 4; ++i) {
        low[i] = (tag >> i) & 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        low[kBit0 + 8 * lane + i] = (byte >> i) & 1;
    }
    low[kTableTrit] = table;
    return make_value(low, 1);
}

Value table_mask(unsigned lane, unsigned tag, unsigned table) {
    // Per class (docs/hla.md 4.4): the lane's bits 0 (bit b becomes trit b), the other bits 1
    // (both bits become 1), the fixed trits and the head their final value (0 or 1).
    std::vector<unsigned> low(kWidth, 1);
    for (unsigned i = 0; i < 4; ++i) {
        low[i] = (tag >> i) & 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        low[kBit0 + 8 * lane + i] = 0;
    }
    low[kTableTrit] = table;
    return make_value(low, 1);
}

constexpr unsigned sign_tag(unsigned lane) { return 8 | (lane & 1) << 1 | (lane >> 1) << 2; }

} // namespace

Value lane_index(unsigned lane, unsigned tag, std::uint8_t byte) {
    return table_index(lane, tag, byte, 1);
}

Value lane_mask(unsigned lane, unsigned tag) { return table_mask(lane, tag, 1); }

Value sign_index(unsigned lane, std::uint8_t byte) {
    return table_index(lane, sign_tag(lane), byte, 0);
}

Value sign_mask(unsigned lane) { return table_mask(lane, sign_tag(lane), 0); }

Value in_seed() {
    // The trits 0..5 swap 0 and 1 of the byte (injective), the trit 35 marks the table, the rest
    // is 0, so a byte gives head 1 and EOF (all trits 2) head 0.
    std::vector<unsigned> low(kWidth, 0);
    for (unsigned i = 0; i < 6; ++i) {
        low[i] = 1;
    }
    low[35] = 2;
    return make_value(low, 0);
}

Value in_index(int byte_or_eof) {
    assert(byte_or_eof >= -1 && byte_or_eof <= 255);
    const Value a = byte_or_eof < 0 ? *Value::from_int(-1)
                                    : *Value::natural(static_cast<std::uint64_t>(byte_or_eof));
    return crz(a, in_seed());
}

Value switch_mask(std::uint32_t s) {
    assert(s < kMaxSwitches);
    // On the lane-0 index of tag 1: trit 0 (1) becomes 2, the trits 1..3 (0) stay 0, the byte's
    // bits (0/1) become 0/2, the other lanes' bits (1) spell s, the rest (1) stays 1.
    std::vector<unsigned> low(kWidth, 0);
    low[0] = 2;
    for (unsigned i = 1; i < 4; ++i) {
        low[i] = 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        low[kBit0 + i] = 2;
    }
    for (unsigned i = 0; i < 24; ++i) {
        low[kBit0 + 8 + i] = (s >> i) & 1 ? 0 : 1;
    }
    return make_value(low, 0);
}

Value switch_index(std::uint32_t s, std::uint8_t b) {
    return crz(switch_mask(s), lane_index(0, kSwitchTag, b));
}

namespace {

// A record or frame: the trits 0..3 given, the bit i of `bits` as the trit 4 + i = 1 - bit, the
// trit 36 = 0, the trit 37 and the head 1.
Value record(std::array<unsigned, 4> low4, std::uint32_t bits) {
    std::vector<unsigned> low(kWidth, 1);
    for (unsigned i = 0; i < 4; ++i) {
        low[i] = low4[i];
    }
    for (unsigned i = 0; i < kWordBits; ++i) {
        low[kBit0 + i] = 1 - ((bits >> i) & 1);
    }
    low[kTableTrit] = 0;
    return make_value(low, 1);
}

// A per-class constant: `bits` on the bit trits, `low` on the trits 0..3 and 36, `high` on the
// trit 37 and the head.
Value classes(unsigned bits, unsigned low, unsigned high) {
    std::vector<unsigned> t(kWidth, high);
    for (unsigned i = 0; i < kWidth - 1; ++i) {
        t[i] = i >= kBit0 && i < kBit0 + kWordBits ? bits : low;
    }
    return make_value(t, high);
}

} // namespace

Value ram_record(std::uint32_t addr) { return record({2, 0, 0, 0}, addr); }

Value ram_pointer(std::uint32_t addr) { return Value::from_raw(ram_record(addr).raw() - 2); }

// With X a word cell: A := crz(K, X) has the bits 0 -> 0, 1 -> 1, the low trits 1, the high 0;
// crz(A, C1) maps them to 1 - bit, 0 and 1. R turns X' back into X.
Value pointer_mask() { return classes(2, 0, 1); }
Value pointer_restore() { return classes(2, 1, 1); }

Value frame_record(std::uint32_t s) { return record({2, 0, 1, 0}, s); }

Value code_address(std::uint32_t id) {
    assert(id >= 1);
    return Value::from_raw(word(id).raw() + 2 * pow3[kTableTrit]);
}

// Arithmetic. The sequences below come from a cheapest-first search over SET/OPR sequences (as in
// prototypes/word32/search.py); tests/hla/word_test.cpp checks every one by simulation.

namespace {

const Value kC1 = make_value({}, 1);

Step set(Value k) { return {-1, k}; }
Step opr(int cell) { return {cell, Value{}}; }

// `bits` on the bit trits, `rest` on the other trits and the head.
Value per_class(unsigned bits, unsigned rest) {
    return trits([&](unsigned i) { return is_bit_trit(i) ? bits : rest; });
}

// The trit of a nibble index at position i outside the nibble's digits: the tag (0, cin, k mod 3,
// k / 3), the table trit 2, 1 on the other bits and the high trits; -1 on the nibble's digits.
int nibble_fixed(unsigned k, unsigned cin, unsigned i) {
    switch (i) {
    case 0:
        return 0;
    case 1:
        return static_cast<int>(cin);
    case 2:
        return static_cast<int>(k % 3);
    case 3:
        return static_cast<int>(k / 3);
    case kTableTrit:
        return 2;
    default:
        return is_bit_trit(i) && (i - kBit0) / kNibbleBits == k ? -1 : 1;
    }
}

// The constants of the mask sequence of nibble k: j = 0 the first, 1 the last (for the carry-in).
Value mask_constant(unsigned k, unsigned j, unsigned cin) {
    // Per class (first, last): the nibble's digits (0, 2), the other bits (1, 0), the trits 37 and
    // up (0, 0) end as 1; a tag trit ends as 0 with (0, 1), 1 with (0, 0), 2 with (2, 0). The carry
    // trit is 0 or 1, so only the last constant depends on it.
    return trits([&](unsigned i) -> unsigned {
        if (i >= kWidth - 1) {
            return 0;
        }
        if (is_bit_trit(i)) {
            return (i - kBit0) / kNibbleBits == k ? std::array<unsigned, 2>{0, 2}[j]
                                                  : std::array<unsigned, 2>{1, 0}[j];
        }
        switch (nibble_fixed(k, cin, i)) {
        case 0:
            return std::array<unsigned, 2>{0, 1}[j];
        case 1:
            return 0;
        default:
            return std::array<unsigned, 2>{2, 0}[j];
        }
    });
}

} // namespace

Sequence and_sequence() {
    return {set(per_class(2, 0)), opr(2), set(kC1), opr(0),   opr(2), opr(2),
            set(Value{}),         opr(1), opr(2),   set(kC1), opr(1)};
}

Sequence or_sequence() {
    // Z := C1 first (which leaves A = C1); then x | y.
    return {set(kC1), opr(2),
            opr(2),   opr(0),
            opr(2),   set(per_class(2, 0)),
            opr(1),   opr(2),
            opr(2),   set(per_class(2, 1)),
            opr(1),   set(per_class(2, 1)),
            opr(2)};
}

Sequence xor_sequence() {
    return {set(kC1), opr(2), opr(2), opr(0), opr(2), set(kC1), opr(1), opr(2), set(kC1), opr(2)};
}

unsigned sigma(unsigned digit) {
    assert(digit < 3);
    return std::array<unsigned, 3>{1, 2, 0}[digit];
}

Sequence sum_sequence(const Digits& d) {
    // Per class, three constants: x + y (1, 2, 2), x + 1 - y (0, 1, 2); the other trits (0, 0, 0)
    // keep S at 1.
    std::array<Value, 3> c;
    for (unsigned j = 0; j < 3; ++j) {
        c[j] = trits([&](unsigned i) -> unsigned {
            if (!is_bit_trit(i)) {
                return 0;
            }
            const Digit g = d[i - kBit0];
            assert(g == Digit::Add || g == Digit::Sub);
            return g == Digit::Add ? std::array<unsigned, 3>{1, 2, 2}[j]
                                   : std::array<unsigned, 3>{0, 1, 2}[j];
        });
    }
    return {set(c[0]), opr(2),   set(kC1), opr(1), opr(2),    set(c[1]),
            opr(2),    set(kC1), opr(0),   opr(2), set(c[2]), opr(2)};
}

Sequence sum_sequence_imm(const Digits& d) {
    // Per class, two constants: x (2, 2), x + 1 (1, 2), 1 - x (0, 2), 2 - x (0, 1); the other
    // trits (1, 0).
    std::array<Value, 2> c;
    for (unsigned j = 0; j < 2; ++j) {
        c[j] = trits([&](unsigned i) -> unsigned {
            if (!is_bit_trit(i)) {
                return j == 0 ? 1 : 0;
            }
            switch (d[i - kBit0]) {
            case Digit::X:
                return 2;
            case Digit::XPlus1:
                return j == 0 ? 1 : 2;
            case Digit::NotX:
                return j == 0 ? 0 : 2;
            case Digit::NotXPlus1:
                return j == 0 ? 0 : 1;
            default:
                assert(false && "a register digit in a sum with a constant");
                return 0;
            }
        });
    }
    return {set(c[0]), opr(1), set(kC1), opr(0), opr(1), set(c[1]), opr(1)};
}

Value nibble_index(unsigned k, unsigned cin, const std::array<unsigned, kNibbleBits>& d) {
    assert(k < kNibbles && cin < 2);
    return trits([&](unsigned i) -> unsigned {
        const int f = nibble_fixed(k, cin, i);
        if (f >= 0) {
            return static_cast<unsigned>(f);
        }
        // An even nibble sees the sums as written, an odd one swapped (1 <-> 2) by the masks
        // before it; the same constants then give these two digit codes.
        const unsigned digit = d[(i - kBit0) % kNibbleBits];
        return k % 2 == 0 ? std::array<unsigned, 3>{1, 0, 2}[digit] : digit;
    });
}

Sequence mask_prefix(unsigned k) {
    // The one access to S, with A = 2 on the bits, swaps its digit trits 1 <-> 2 and reads them.
    return {set(mask_constant(k, 0, 0)), opr(1), set(per_class(2, 0)), opr(0), opr(1)};
}

Sequence mask_last(unsigned k, unsigned cin) { return {set(mask_constant(k, 1, cin)), opr(1)}; }

Sequence transform(const TritMap& m) {
    bool short_form = true;
    for (const Trit t : m) {
        short_form = short_form && (t == Trit::Keep || t == Trit::Clear);
    }
    // Per class (keep, clear, fill, flip), the head last.
    const auto constant = [&](std::array<unsigned, 4> per, unsigned head) {
        return trits(
            [&](unsigned i) { return i < kWidth ? per[static_cast<unsigned>(m[i])] : head; });
    };
    if (short_form) {
        // keep 1, clear 2, then C1: a 2 becomes 1 and then 0, a 0 stays 0.
        return {set(constant({1, 2, 0, 0}, 1)), opr(0), set(kC1), opr(0)};
    }
    // keep (0, 2), clear (0, 1), fill (1, 2), flip (2, 2), the head (1, 1).
    return {set(constant({0, 0, 1, 2}, 1)), opr(0), opr(0), set(constant({2, 1, 2, 2}, 1)), opr(0)};
}

TritMap shift_map(bool left, unsigned k) {
    assert(k < kWordBits);
    // The result's bits: k .. 31 after a left shift, 0 .. 31 - k after a right one.
    TritMap m;
    for (unsigned t = 0; t < kWidth; ++t) {
        const bool bit = is_bit_trit(t) && (left ? t - kBit0 >= k : t - kBit0 + k < kWordBits);
        m[t] = bit ? Trit::Keep : Trit::Clear;
    }
    return m;
}

} // namespace malm::hla
