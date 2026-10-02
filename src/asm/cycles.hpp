#pragma once

// Per-address cycle and placement tables for the assembler (issue #12; HeLL-style code placement,
// research 1.3-1.6 and 4.3, spec 3.1, 3.5 and 5).
//
// "Generator" in the issue means generated, not hand-written: every table here is computed at
// compile time from kXlat1 / kXlat2 (constexpr initializers), so there is no hand-typed data and no
// separate build-time tool. The namespace is `assembler` because `asm` is a keyword.
//
// Model. A cell at address a holds a character c in 33..126. Only a = position mod 94 matters
// ("position" p below). Executing the cell decodes it at p (spec 5 step 2): decode(p, c). Executing
// any instruction other than Jmp then encrypts the cell, c := xlat2(c) (spec 5 step 4), so the
// instruction a cell performs over successive executions is
//     decode(p, c), decode(p, xlat2(c)), decode(p, xlat2^2(c)), ...
// and this sequence is periodic with the length of c's xlat2 cycle. xlat2 is a permutation
// of 33..126 with six cycles, of lengths 2, 4, 5, 6, 9 and 68 (research 1.6).
//
// Restore semantics (what the assembler's R_ labels and restore helpers rely on; spec 5 steps 3-4).
//  * After a non-Jmp instruction executes at address a, the cell at a is encrypted: one xlat2 step.
//    After L executions of a cell in an L-cycle, with no Jmp in between, the cell is back at its
//    original character. Running a cell L - k more times undoes k executions ("restore").
//  * A Jmp sets C := [D] in step 3, and step 4 encrypts the cell at that NEW C, not the Jmp
//    itself. Execution then resumes at C + 1. So a jump that should resume at label X needs
//    [D] = X - 1, and it is the cell at X - 1 that is encrypted, once. The Jmp cell is never
//    encrypted by its own execution, which is why a cell that has become a Jmp stays a Jmp.
//  * Consequence: the cell encrypted by "jump and resume at X" is X - 1, so code between the jump
//    and X is not advanced, and a cell at X - 1 can be advanced (or restored, after L - 1 more
//    steps in an L-cycle) as a side effect of the jump. The Jmp cell itself keeps its state.
//  Assembler logic is not part of this header; it only exposes the tables.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/placement.hpp"
#include "core/tables.hpp"

namespace malm::assembler {

using core::Op;

// Loadable placement: the unique character in 33..126 that is loadable at position p and decodes to
// op there (reuses core::denormalize). Op::Nop is the instruction letter 'o'.
using core::loadable_char;

inline constexpr std::size_t kNumCycles = 6;
inline constexpr std::size_t kMaxCycleLength = 68;
inline constexpr std::size_t kMaxFlagLength = 9;

namespace detail {

constexpr std::size_t kN = 94;

// Plain structs with raw arrays: std::array::operator[] is an extra call per access under constant
// evaluation, which pushes big tables over clang's constexpr step limit.
struct CycleTables {
    char members[kN];                   // all chars, grouped by cycle, each in cycle order
    std::uint8_t begin[kNumCycles + 1]; // cycle i occupies members[begin[i] .. begin[i+1])
    std::uint8_t id[kN];                // by c - 33
    std::uint8_t index[kN];             // position of c within its cycle
    std::uint8_t flat[kN];              // position of c in members
};

constexpr CycleTables make_cycle_tables() {
    CycleTables t{};
    std::uint8_t len_of[kN]{};
    for (std::size_t i = 0; i < kN; ++i) {
        std::size_t x = i;
        std::size_t len = 0;
        do {
            x = static_cast<std::size_t>(static_cast<unsigned char>(core::kXlat2[x])) - 33;
            ++len;
        } while (x != i);
        len_of[i] = static_cast<std::uint8_t>(len);
    }
    // Cycles ordered by length, then by smallest member; each starts at its smallest member.
    bool placed[kN]{};
    std::size_t out = 0;
    std::size_t cyc = 0;
    for (std::size_t len = 1; len <= kN; ++len) {
        for (std::size_t i = 0; i < kN; ++i) {
            if (len_of[i] != len || placed[i]) {
                continue;
            }
            t.begin[cyc] = static_cast<std::uint8_t>(out);
            std::size_t x = i;
            for (std::size_t k = 0; k < len; ++k) {
                placed[x] = true;
                t.members[out] = static_cast<char>(x + 33);
                t.id[x] = static_cast<std::uint8_t>(cyc);
                t.index[x] = static_cast<std::uint8_t>(k);
                t.flat[x] = static_cast<std::uint8_t>(out);
                ++out;
                x = static_cast<std::size_t>(static_cast<unsigned char>(core::kXlat2[x])) - 33;
            }
            ++cyc;
        }
    }
    t.begin[cyc] = static_cast<std::uint8_t>(out);
    return t;
}

inline constexpr CycleTables kCycles = make_cycle_tables();

static_assert(kCycles.begin[kNumCycles] == kN, "xlat2 must have exactly six cycles");

// kOps[p][f]: decode(p, members[f]).
struct OpTable {
    Op v[kN][kN];
};

constexpr OpTable make_op_table() {
    OpTable t{};
    for (std::size_t p = 0; p < kN; ++p) {
        for (std::size_t f = 0; f < kN; ++f) {
            t.v[p][f] = core::decode(
                static_cast<std::uint32_t>(p),
                static_cast<std::uint32_t>(static_cast<unsigned char>(kCycles.members[f])));
        }
    }
    return t;
}

inline constexpr OpTable kOps = make_op_table();

// kImmutable.v[p][c - 33]: every element of c's cycle decodes to Nop at p.
struct ImmutableTable {
    bool v[kN][kN];
};

constexpr ImmutableTable make_immutable_table() {
    ImmutableTable t{};
    for (std::size_t p = 0; p < kN; ++p) {
        for (std::size_t cyc = 0; cyc < kNumCycles; ++cyc) {
            bool all_nop = true;
            for (std::size_t f = kCycles.begin[cyc]; f < kCycles.begin[cyc + 1]; ++f) {
                all_nop = all_nop && kOps.v[p][f] == Op::Nop;
            }
            for (std::size_t f = kCycles.begin[cyc]; f < kCycles.begin[cyc + 1]; ++f) {
                t.v[p][static_cast<std::size_t>(static_cast<unsigned char>(kCycles.members[f])) -
                       33] = all_nop;
            }
        }
    }
    return t;
}

inline constexpr ImmutableTable kImmutable = make_immutable_table();

constexpr std::size_t slot(std::uint8_t c) noexcept { return static_cast<std::size_t>(c) - 33; }

} // namespace detail

// ---- xlat2 cycle structure (position independent). Precondition: c in 33..126. ----

// Cycle id 0..5, ordered by increasing length: lengths 2, 4, 5, 6, 9, 68.
constexpr std::size_t cycle_id(std::uint8_t c) noexcept {
    return detail::kCycles.id[detail::slot(c)];
}
// Index of c within its cycle (the cycle starts at its smallest character).
constexpr std::size_t cycle_index(std::uint8_t c) noexcept {
    return detail::kCycles.index[detail::slot(c)];
}
constexpr std::size_t cycle_length(std::uint8_t c) noexcept {
    const std::size_t id = cycle_id(c);
    return static_cast<std::size_t>(detail::kCycles.begin[id + 1] - detail::kCycles.begin[id]);
}
constexpr std::size_t cycle_length_of_id(std::size_t id) noexcept {
    return static_cast<std::size_t>(detail::kCycles.begin[id + 1] - detail::kCycles.begin[id]);
}
// The members of cycle `id` in cycle order: m[i + 1] == xlat2(m[i]), wrapping around.
constexpr std::string_view cycle_members(std::size_t id) noexcept {
    return {detail::kCycles.members + detail::kCycles.begin[id], cycle_length_of_id(id)};
}
// xlat2^k(c); k is taken mod the cycle length.
constexpr std::uint8_t cycle_next(std::uint8_t c, std::size_t k = 1) noexcept {
    const std::size_t id = cycle_id(c);
    const std::size_t len = cycle_length_of_id(id);
    return static_cast<std::uint8_t>(
        detail::kCycles.members[detail::kCycles.begin[id] + (cycle_index(c) + k) % len]);
}

// ---- Per-position instruction sequence. Preconditions: p < 94, c in 33..126. ----

// decode(p, xlat2^k(c)); k is taken mod the cycle length of c. One table lookup.
constexpr Op op_at(std::size_t p, std::uint8_t c, std::size_t k) noexcept {
    const std::size_t id = cycle_id(c);
    const std::size_t begin = detail::kCycles.begin[id];
    const std::size_t len = cycle_length_of_id(id);
    return detail::kOps.v[p][begin + (cycle_index(c) + k) % len];
}

// The instruction sequence of a cell starting at c: ops[k] = op_at(p, c, k) for k < length.
struct OpSequence {
    std::array<Op, kMaxCycleLength> ops{};
    std::size_t length = 0;
    constexpr Op operator[](std::size_t k) const noexcept { return ops[k % length]; }
};

constexpr OpSequence op_sequence(std::size_t p, std::uint8_t c) noexcept {
    OpSequence s;
    s.length = cycle_length(c);
    for (std::size_t k = 0; k < s.length; ++k) {
        s.ops[k] = op_at(p, c, k);
    }
    return s;
}

// ---- Immutable Nops. ----

// Every element of c's cycle decodes to Nop at p. Op::Nop covers both the instruction 'o' and every
// non-instruction code, so such a cell is a Nop forever.
constexpr bool is_immutable_nop(std::size_t p, std::uint8_t c) noexcept {
    return detail::kImmutable.v[p][detail::slot(c)];
}

// The character loadable at p (the instruction 'o') if it is an immutable Nop there, else none.
constexpr std::optional<std::uint8_t> immutable_nop_char(std::size_t p) noexcept {
    const std::uint8_t c = loadable_char(p, Op::Nop);
    if (is_immutable_nop(p, c)) {
        return c;
    }
    return std::nullopt;
}

// The character an `RNop` (any all-Nop cycle, docs/asm.md 3.1) assembles to at p: the loadable 'o'
// if it is an immutable Nop there, otherwise the smallest immutable-Nop character. One exists at
// every position, so this always succeeds.
constexpr std::uint8_t rnop_char(std::size_t p) noexcept {
    if (const auto c = immutable_nop_char(p)) {
        return *c;
    }
    for (unsigned c = 33; c <= 126; ++c) {
        if (is_immutable_nop(p, static_cast<std::uint8_t>(c))) {
            return static_cast<std::uint8_t>(c);
        }
    }
    return 0; // unreachable: checked for every position by a static_assert below
}

namespace detail {
constexpr bool rnop_everywhere() {
    for (std::size_t p = 0; p < kN; ++p) {
        if (rnop_char(p) == 0) {
            return false;
        }
    }
    return true;
}
} // namespace detail
static_assert(detail::rnop_everywhere(), "an immutable Nop exists at every position");

// ---- Flags. ----

// Some element of c's cycle is loadable at p, i.e. a cell holding c can be reached from a loaded
// program (loaded directly, or after some encryption steps).
constexpr bool is_reachable(std::size_t p, std::uint8_t c) noexcept {
    const std::size_t id = cycle_id(c);
    for (std::size_t f = detail::kCycles.begin[id]; f < detail::kCycles.begin[id + 1]; ++f) {
        if (core::is_loadable(static_cast<std::uint32_t>(p),
                              static_cast<std::uint32_t>(
                                  static_cast<unsigned char>(detail::kCycles.members[f])))) {
            return true;
        }
    }
    return false;
}

// A flag candidate: a character whose cycle at p has length `length` (<= 9) and the instruction
// each execution state performs: ops[k] = decode(p, xlat2^k(ch)) for k < length.
struct FlagCandidate {
    std::uint8_t ch = 0;
    std::size_t length = 0;
    std::array<Op, kMaxFlagLength> ops{};
};

struct FlagCandidates {
    std::array<FlagCandidate, kMaxFlagLength> items{};
    std::size_t count = 0;
};

// Flag candidates for instruction `op` at p with cycle length L in {2, 4, 5, 6, 9}: all characters
// ch, reachable at p, whose cycle has length L and whose decoded sequence at p contains both `op`
// and Nop (a flag toggles between doing `op` and doing nothing). Characters are listed in the
// cycle order of the cycle (all rotations of the same cycle appear, as distinct starting states).
// At most one cycle has a given length in {2, 4, 5, 6, 9}, so the result has at most L entries.
// Empty for op == Nop or any other L.
constexpr FlagCandidates flag_candidates(std::size_t p, Op op, std::size_t length) noexcept {
    FlagCandidates r;
    if (op == Op::Nop || length < 2 || length > kMaxFlagLength) {
        return r;
    }
    for (std::size_t id = 0; id < kNumCycles; ++id) {
        if (cycle_length_of_id(id) != length) {
            continue;
        }
        const std::size_t begin = detail::kCycles.begin[id];
        bool has_op = false;
        bool has_nop = false;
        for (std::size_t k = 0; k < length; ++k) {
            has_op = has_op || detail::kOps.v[p][begin + k] == op;
            has_nop = has_nop || detail::kOps.v[p][begin + k] == Op::Nop;
        }
        const auto first = static_cast<std::uint8_t>(detail::kCycles.members[begin]);
        if (!has_op || !has_nop || !is_reachable(p, first)) {
            continue;
        }
        for (std::size_t k = 0; k < length; ++k) {
            FlagCandidate& f = r.items[r.count++];
            f.ch = static_cast<std::uint8_t>(detail::kCycles.members[begin + k]);
            f.length = length;
            for (std::size_t j = 0; j < length; ++j) {
                f.ops[j] = op_at(p, f.ch, j);
            }
        }
    }
    return r;
}

// Two-state flags: flag_candidates with L == 2. For L == 2 the cycle is F(70) <-> J(74), so one
// state decodes to `op` and the other to Nop.
constexpr FlagCandidates flag_chars(std::size_t p, Op op) noexcept {
    return flag_candidates(p, op, 2);
}

} // namespace malm::assembler
