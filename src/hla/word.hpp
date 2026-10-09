#pragma once

// 32-bit words and the constants the HLA lowering works with (docs/hla.md 4,
// docs/adr/0001-hla-word-representation.md).
//
// A word w is one cell: bit i is trit 4 + i, stored as 0 or 2 ("f02"); the trits 0..3, 36 and above
// and the head are 0. With A = C1, crz is the identity on such a cell, which is how a register is
// read into A without changing it. HLA programs run at width 38.
//
// Tables. The lowering dispatches on a byte through tables of the head-1 window (docs/hla.md 4.4):
//  * lane tables: the index of a register's byte lane (lane_index) has the lane's 8 bits as the
//    trits 0/1, the other bit trits 1, the trits 36, 37 and the head 1 and a tag in the trits 0..3
//    (each 0 or 1), so different tables never share an index; consecutive indices are 81 apart;
//  * the input table: In leaves a byte b (or EOF) in A, and crz(b, in_seed()) spreads it over 729
//    consecutive cells with the trit 35 = 2, which no lane index has. EOF lands in head 0;
//  * switch tables: a lane-0 index (tag kSwitchTag) crazied with switch_mask(s) has the byte's bits
//    as the trits 0/2, the switch number in the trits 12..35 (0/1, so trit 35 is never 2), the
//    trits 0..3 = 2, 0, 0, 0 (no lane index has a 2 there) and the trits 36, 37 and the head 1.
//
// Records. Every other structure the lowering pins lies apart from the tables:
//  * RAM words (ADR 0001): the word at byte address a is the value cell of a record
//    `[value][MB][BACK]` at ram_record(a): the trits 0..3 = 2, 0, 0, 0, the bit i of a as the trit
//    4 + i = 1 - bit, the trit 36 = 0 (tables have 1), the trit 37 and the head 1. A pointer
//    register holds ram_pointer(a) = record - 2 and is formed from the address by crz alone
//    (pointer_mask);
//  * stack frames: the same shape with the trits 0..3 = 2, 0, 1, 0, so they never meet a RAM
//  record;
//  * code addresses (label values, return addresses): f02 values with head 0, the trit 36 = 2 and
//    an id in the bit trits; a MovD through one lands on its pad at code_address(id) + 1, far above
//    everything the assembler places first-fit in head 0.
//
// Arithmetic (docs/hla.md 4.6, docs/adr/0002-hla-arithmetic.md) adds two table families:
//  * nibble tables: the index of nibble k (bits 4k .. 4k + 3) of a column sum has the nibble's
//    digits 0..2 as its trits, the other bits 1, a tag for (k, carry-in) in the trits 0..3 and the
//    trit 36 = 2, which nothing else in head 1 has;
//  * sign tables: lane indices with the trit 36 = 0 (like records, which have a 2 in the trits
//    0..3, where these have a tag of 0s and 1s).

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/value.hpp"

namespace malm::hla {

using core::Value;

inline constexpr unsigned kWidth = 38; // the program width of every HLA program
inline constexpr unsigned kBit0 = 4;   // trit of bit 0
inline constexpr unsigned kWordBits = 32;
inline constexpr unsigned kLanes = 4;
inline constexpr unsigned kSwitchTag = 1;
inline constexpr std::uint32_t kMaxSwitches = (std::uint32_t{1} << 24) - 1;

// A value from its low trits (trit 0 first) and its head; the trits above `low` equal the head.
Value make_value(const std::vector<unsigned>& low, unsigned head);

// The cell of w, and back (nullopt if `v` is not a word cell).
Value word(std::uint32_t w);
std::optional<std::uint32_t> word_value(Value v);

// The `a` with crz(a, from) == to, if one exists (a trit 0 in `from` cannot become 2).
std::optional<Value> crz_operand(Value from, Value to);

// Loading a constant into A without changing any cell (docs/asm.md 10.3): Rot of C0 or C1 (both
// are rotation fixed points), then Opr on inline constants, each a fixed point of the A it meets.
// At most four Oprs are needed for any value of width <= 38 with head 0 or 1.
struct Load {
    Value start;             // C0 or C1
    std::vector<Value> oprs; // the inline constants, in order
};
Load load(Value target);

// Lane tables (see above). The tag is 4 bits (trits 0..3).
Value lane_index(unsigned lane, unsigned tag, std::uint8_t byte);
// The constant K of the index sequence (docs/hla.md 4.4):
//   A := C1; Opr I; Opr I; Opr X; Opr I; A := K; Opr I; Opr I
// leaves I = lane_index(lane, tag, byte of X) and X unchanged.
Value lane_mask(unsigned lane, unsigned tag);

// The input table: the cell the dispatch crazies In's value into, and its value afterwards for a
// byte (0..255) or for EOF (-1).
Value in_seed();
Value in_index(int byte_or_eof);

// Switch tables (see above): A := switch_mask(s); Opr I turns lane_index(0, kSwitchTag, b) into
// switch_index(s, b). s < kMaxSwitches.
Value switch_mask(std::uint32_t s);
Value switch_index(std::uint32_t s, std::uint8_t b);

// RAM (see above). `addr` is a byte address; only multiples of 4 have a record.
Value ram_record(std::uint32_t addr);
Value ram_pointer(std::uint32_t addr);
// The pointer sequence: P := C1 (reset); A := pointer_mask(); Opr X; Opr P; A := pointer_restore();
// Opr X leaves P = ram_pointer(word_value(X)) and X unchanged.
Value pointer_mask();
Value pointer_restore();

// Stack frame s (see above); the stack pointer holds frame_record(s) - 2.
Value frame_record(std::uint32_t s);

// The code address with the given id (>= 1); its pad is at code_address(id) + 1.
Value code_address(std::uint32_t id);

// Arithmetic. A sequence is a list of loads (SET k: A := k) and accesses (OPR c: A := c :=
// crz(A, c)) on whole cells, numbered per sequence. crz acts on every trit on its own, so a
// constant with different trits on different bits makes one sequence do different things to
// different bits.
struct Step {
    int cell; // the accessed cell, or -1 for a load
    Value k;  // the load's constant
};
using Sequence = std::vector<Step>;

// A constant from its trits: f(i) for i < kWidth, the head f(kWidth) (0 or 1).
template <class F> Value trits(F f) {
    std::vector<unsigned> low(kWidth);
    for (unsigned i = 0; i < kWidth; ++i) {
        low[i] = f(i);
    }
    return make_value(low, f(kWidth));
}
// The trit of bit i, and whether trit t holds a bit.
constexpr unsigned bit_trit(unsigned i) { return kBit0 + i; }
constexpr bool is_bit_trit(unsigned t) { return t >= kBit0 && t < kBit0 + kWordBits; }
// Bitwise operations: Z := X op Y on words, X and Y kept (cells 0 = X, 1 = Y, 2 = Z). X and Y may
// be the same cell; Z must be another one, and may hold any word before.
Sequence and_sequence();
Sequence or_sequence();
Sequence xor_sequence();

// Column sums. The digit of bit i is a function of x_i (and y_i) in 0..2; the sum cell S holds
// sigma(digit) on the bit trits and 1 elsewhere. S is C1 before.
//  * of two registers (cells 0 = X, 1 = Y, 2 = S; X and Y are only read, with A = C1, so they may
//    be the same cell): the digit x + y (Add) or x + 1 - y (Sub) per bit;
//  * of a register and a constant (cells 0 = X, 1 = S): the digit x, x + 1, 1 - x or 2 - x.
enum class Digit : std::uint8_t { Add, Sub, X, XPlus1, NotX, NotXPlus1 };
using Digits = std::array<Digit, kWordBits>;
Sequence sum_sequence(const Digits& d);
Sequence sum_sequence_imm(const Digits& d);
unsigned sigma(unsigned digit); // 1, 2, 0

// Nibble tables. Nibble k is bits 4k .. 4k + 3; the add runs over the nibbles with a carry.
inline constexpr unsigned kNibbles = 8;
inline constexpr unsigned kNibbleBits = 4;
// The index of nibble k with carry-in cin and the column-sum digits `d` (bit 4k first).
Value nibble_index(unsigned k, unsigned cin, const std::array<unsigned, kNibbleBits>& d);
// The mask sequence from the sum cell S to the index cell J (cells 0 = S, 1 = J; J is C1 before):
// the prefix, then the last step for the carry-in, leave J = nibble_index(k, cin, the nibble's
// digits). Only the last step depends on the carry. Each mask swaps the digit trits of S (1 <-> 2),
// so the masks of nibbles 0, 1, ..., 7 in order find S as the sums made it, swapped, and so on,
// and leave it as the sums made it.
Sequence mask_prefix(unsigned k);
Sequence mask_last(unsigned k, unsigned cin);

// In-place transforms of a cell H (cell 0) whose trits 0..37 are 0 or 2 and whose head is 0: each
// trit is kept, cleared (0), filled (2) or flipped; the head stays 0. Two accesses with keep and
// clear only, else three.
enum class Trit : std::uint8_t { Keep, Clear, Fill, Flip };
using TritMap = std::array<Trit, kWidth>;
Sequence transform(const TritMap& m);
// Bit i of a word maps to `f(i)`; the trits outside the bits are kept (they are 0 in a word).
template <class F> TritMap bit_map(F f) {
    TritMap m;
    m.fill(Trit::Keep);
    for (unsigned i = 0; i < kWordBits; ++i) {
        m[bit_trit(i)] = f(i);
    }
    return m;
}

// Shifts. Rot rotates the trits 0..kWidth - 1 right by one, so a shift by k (1..31) is k rotations
// right or kWidth - k for a left shift; shift_map then clears the trits that are not bits of the
// result (the bits that fell off, rotated around, and the zeros that came in). Bits fill with 0.
constexpr unsigned rotations(bool left, unsigned k) { return k == 0 ? 0 : left ? kWidth - k : k; }
TritMap shift_map(bool left, unsigned k);

// Sign tables: the index of lane l of a word with the trit 36 = 0 and the tag sign_tag(l); the
// lane index sequence with sign_mask(l) (instead of lane_mask) builds it.
Value sign_index(unsigned lane, std::uint8_t byte);
Value sign_mask(unsigned lane);

} // namespace malm::hla
